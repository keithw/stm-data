#include <pty.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <locale.h>
#include <langinfo.h>
#include <wchar.h>
#include <assert.h>
#include <wctype.h>
#include <iostream>
#include <typeinfo>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/time.h>

#include "swrite.hpp"

const size_t buf_size = 16384;

const char HOST_STRING[] = "HOST";
const char USER_STRING[] = "USER";

void save_session( int log_fd, int child_fd );

uint64_t micro_timestamp( void )
{
  struct timespec tp;

  assert( clock_gettime( CLOCK_MONOTONIC, &tp ) >= 0 );

  uint64_t micros = tp.tv_nsec / 1000;
  micros += uint64_t( tp.tv_sec ) * 1000000;

  return micros;
}

void record_string( int log_fd, char *buf, ssize_t num, const char *tag )
{
  char message[ 2048 ];
  int len = snprintf( message, 2048, "%ld %s %ld\n", micro_timestamp(), tag, num );
  assert( len >= 0 );
  assert( len < 2048 );

  swrite( log_fd, message, len );
  swrite( log_fd, buf, num );
  swrite( log_fd, "\n" );
}

void record_resize( int log_fd, int width, int height )
{
  char message[ 2048 ];
  int len = snprintf( message, 2048, "%ld SIZE %d %d\n", micro_timestamp(), width, height );
  assert( len >= 0 );
  assert( len < 2048 );
  swrite( log_fd, message, len );
}

int main( int argc, char *argv[] )
{
  if ( argc != 2 ) {
    fprintf( stderr, "USAGE: %s LOGFILE\n", argv[ 0 ] );
    exit( 1 );
  }

  int log_fd = open( argv[ 1 ], O_WRONLY | O_CREAT | O_TRUNC | O_NOCTTY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH );
  if ( log_fd < 0 ) {
    perror( "creat" );
    exit( 1 );
  }

  int master;
  struct termios saved_termios, raw_termios, child_termios;

  if ( NULL == setlocale( LC_ALL, "" ) ) {
    perror( "setlocale" );
    exit( 1 );
  }

  if ( strcmp( nl_langinfo( CODESET ), "UTF-8" ) != 0 ) {
    fprintf( stderr, "term-save requires a UTF-8 locale.\n" );
    exit( 1 );
  }

  if ( tcgetattr( STDIN_FILENO, &saved_termios ) < 0 ) {
    perror( "tcgetattr" );
    exit( 1 );
  }

  child_termios = saved_termios;

  if ( !(child_termios.c_iflag & IUTF8) ) {
    fprintf( stderr, "Warning: Locale is UTF-8 but termios IUTF8 flag not set. Setting IUTF8 flag.\n" );
    child_termios.c_iflag |= IUTF8;
  }

  pid_t child = forkpty( &master, NULL, &child_termios, NULL );

  if ( child == -1 ) {
    perror( "forkpty" );
    exit( 1 );
  }

  if ( child == 0 ) {
    /* child */
    if ( setenv( "TERM", "xterm", true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    /* ask ncurses to send UTF-8 instead of ISO 2022 for line-drawing chars */
    if ( setenv( "NCURSES_NO_UTF8_ACS", "1", true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    /* get shell name */
    struct passwd *pw = getpwuid( geteuid() );
    if ( pw == NULL ) {
      perror( "getpwuid" );
      exit( 1 );
    }

    char *my_argv[ 2 ];
    my_argv[ 0 ] = strdup( pw->pw_shell );
    assert( my_argv[ 0 ] );
    my_argv[ 1 ] = NULL;

    if ( execve( pw->pw_shell, my_argv, environ ) < 0 ) {
      perror( "execve" );
      exit( 1 );
    }
    exit( 0 );
  } else {
    /* parent */
    raw_termios = saved_termios;

    cfmakeraw( &raw_termios );

    if ( tcsetattr( STDIN_FILENO, TCSANOW, &raw_termios ) < 0 ) {
      perror( "tcsetattr" );
      exit( 1 );
    }

    char open[] = "\033c";
    char close[] = "\033[!p";

    swrite( STDOUT_FILENO, open );
    save_session( log_fd, master );
    swrite( STDOUT_FILENO, close );

    if ( tcsetattr( STDIN_FILENO, TCSANOW, &saved_termios ) < 0 ) {
      perror( "tcsetattr" );
      exit( 1 );
    }
  }

  if ( close( log_fd ) < 0 ) {
    perror( "close" );
    exit( 1 );
  }

  printf( "[term-save is exiting.]\n" );

  return 0;
}

/* This is the main loop. */

void save_session( int log_fd, int child_fd )
{
  /* establish WINCH fd and start listening for signal */
  sigset_t signal_mask;
  assert( sigemptyset( &signal_mask ) == 0 );
  assert( sigaddset( &signal_mask, SIGWINCH ) == 0 );

  /* stop "ignoring" WINCH signal */
  assert( sigprocmask( SIG_BLOCK, &signal_mask, NULL ) == 0 );

  int winch_fd = signalfd( -1, &signal_mask, 0 );
  if ( winch_fd < 0 ) {
    perror( "signalfd" );
    return;
  }

  /* get current window size */
  struct winsize window_size;
  if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCGWINSZ" );
    return;
  }

  /* tell child process */
  if ( ioctl( child_fd, TIOCSWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCSWINSZ" );
    return;
  }

  record_resize( log_fd, window_size.ws_col, window_size.ws_row );

  struct pollfd pollfds[ 3 ];

  pollfds[ 0 ].fd = STDIN_FILENO;
  pollfds[ 0 ].events = POLLIN;

  pollfds[ 1 ].fd = child_fd;
  pollfds[ 1 ].events = POLLIN;

  pollfds[ 2 ].fd = winch_fd;
  pollfds[ 2 ].events = POLLIN;

  while ( 1 ) {
    int active_fds = poll( pollfds, 3, -1 );
    if ( active_fds < 0 ) {
      perror( "poll" );
      break;
    }

    if ( pollfds[ 0 ].revents & POLLIN ) {
      /* input from user */
      char buf[ buf_size ];

      /* fill buffer if possible */
      ssize_t bytes_read = read( pollfds[ 0 ].fd, buf, buf_size );
      if ( bytes_read == 0 ) { /* EOF */
	return;
      } else if ( bytes_read < 0 ) {
	perror( "read" );
	return;
      }

      record_string( log_fd, buf, bytes_read, USER_STRING );
      
      /* send along to host */
      if ( swrite( pollfds[ 1 ].fd, buf, bytes_read ) < 0 ) {
	break;
      }
    } else if ( pollfds[ 1 ].revents & POLLIN ) {
      /* input from host */
      char buf[ buf_size ];

      /* fill buffer if possible */
      ssize_t bytes_read = read( pollfds[ 1 ].fd, buf, buf_size );
      if ( bytes_read == 0 ) { /* EOF */
	return;
      } else if ( bytes_read < 0 ) {
	perror( "read" );
	return;
      }

      record_string( log_fd, buf, bytes_read, HOST_STRING );

      /* send along to user */
      if ( swrite( pollfds[ 0 ].fd, buf, bytes_read ) < 0 ) {
	break;
      }
    } else if ( pollfds[ 2 ].revents & POLLIN ) {
      /* resize */
      struct signalfd_siginfo info;
      assert( read( winch_fd, &info, sizeof( info ) ) == sizeof( info ) );
      assert( info.ssi_signo == SIGWINCH );

      /* get new size */
      if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
	perror( "ioctl TIOCGWINSZ" );
	return;
      }

      record_resize( log_fd, window_size.ws_col, window_size.ws_row );

      /* tell child process */
      if ( ioctl( child_fd, TIOCSWINSZ, &window_size ) < 0 ) {
	perror( "ioctl TIOCSWINSZ" );
	return;
      }
    } else if ( (pollfds[ 0 ].revents | pollfds[ 1 ].revents)
		& (POLLERR | POLLHUP | POLLNVAL) ) {
      break;
    }
  }
}
