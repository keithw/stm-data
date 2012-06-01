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
#include <sys/socket.h>
#include <netinet/in.h>

#include "swrite.hpp"

const size_t buf_size = 16384;

void save_session( int socket, int child_fd );

int main( int argc, char *argv[] )
{
  if ( argc != 2 ) {
    fprintf( stderr, "USAGE: %s PORT\n", argv[ 0 ] );
    exit( 1 );
  }

  int sock = socket( AF_INET, SOCK_DGRAM, 0 );
  if ( sock < 0 ) {
    perror( "socket" );
    exit( 1 );
  }
  struct sockaddr_in local_addr;
  local_addr.sin_family = AF_INET;
  local_addr.sin_port = htons( atoi( argv[ 1 ] ) );
  local_addr.sin_addr.s_addr = INADDR_ANY;

  if ( bind( sock, (sockaddr *)&local_addr, sizeof( local_addr ) ) < 0 ) {
    perror( "bind" );
    exit( 1 );
  }

  socklen_t addrlen = sizeof( local_addr );

  if ( getsockname( sock, (sockaddr *)&local_addr, &addrlen ) < 0 ) {
    perror( "getsockname" );
  }

  int port = ntohs( local_addr.sin_port );

  assert( port == atoi( argv[ 1 ] ) );

  int master;
  struct termios saved_termios, raw_termios, child_termios;

  if ( NULL == setlocale( LC_ALL, "" ) ) {
    perror( "setlocale" );
    exit( 1 );
  }

  if ( strcmp( nl_langinfo( CODESET ), "UTF-8" ) != 0 ) {
    fprintf( stderr, "term-receive requires a UTF-8 locale.\n" );
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
    save_session( sock, master );
    swrite( STDOUT_FILENO, close );

    if ( tcsetattr( STDIN_FILENO, TCSANOW, &saved_termios ) < 0 ) {
      perror( "tcsetattr" );
      exit( 1 );
    }
  }

  if ( close( sock ) < 0 ) {
    perror( "close" );
    exit( 1 );
  }

  printf( "[term-receive is exiting.]\n" );

  return 0;
}

/* This is the main loop. */

void save_session( int socket, int child_fd )
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

  struct pollfd pollfds[ 4 ];

  pollfds[ 0 ].fd = STDIN_FILENO;
  pollfds[ 0 ].events = POLLIN;

  pollfds[ 1 ].fd = child_fd;
  pollfds[ 1 ].events = POLLIN;

  pollfds[ 2 ].fd = winch_fd;
  pollfds[ 2 ].events = POLLIN;

  pollfds[ 3 ].fd = socket;
  pollfds[ 3 ].events = POLLIN;

  while ( 1 ) {
    int active_fds = poll( pollfds, 4, -1 );
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

      /* tell child process */
      if ( ioctl( child_fd, TIOCSWINSZ, &window_size ) < 0 ) {
	perror( "ioctl TIOCSWINSZ" );
	return;
      }
    } else if ( pollfds[ 3 ].revents & POLLIN ) {
      /* input from fake user */
      char buf[ buf_size ];

      /* fill buffer if possible */
      ssize_t bytes_read = read( pollfds[ 3 ].fd, buf, buf_size );
      if ( bytes_read == 0 ) { /* EOF */
	return;
      } else if ( bytes_read < 0 ) {
	perror( "read" );
	return;
      }

      /* send along to host */
      if ( swrite( pollfds[ 1 ].fd, buf, bytes_read ) < 0 ) {
	break;
      }
    } else if ( (pollfds[ 0 ].revents | pollfds[ 1 ].revents
		 | pollfds[ 2 ].revents | pollfds[ 3 ].revents)
		& (POLLERR | POLLHUP | POLLNVAL) ) {
      break;
    }
  }
}
