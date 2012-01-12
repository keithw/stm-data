source = swrite.cpp term-save.cpp
objects = swrite.o
executables = term-save

CXX = g++
CXXFLAGS = -g -O2 --std=c++0x -pedantic -Werror -Wall -Wextra -Weffc++ -fno-default-inline -pipe -D_FILE_OFFSET_BITS=64 -D_XOPEN_SOURCE=500 -D_GNU_SOURCE -D_BSD_SOURCE
LIBS = -lutil -lrt

all: $(executables)

term-save: term-save.o $(objects)
	$(CXX) $(CXXFLAGS) -o $@ $+ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include depend

depend: $(source)
	$(CXX) $(INCLUDES) -MM $(source) > depend

.PHONY: clean
clean:
	-rm -f $(executables) depend *.o
