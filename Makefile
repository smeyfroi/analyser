TARGET = analyser
INCLUDE = -I/usr/local/include -I/usr/include -I/Gist/src -I/Gist/libs/kiss_fft130
LDFLAGS = -L/Gist/build/src
LIBS = -lGist -lrt
CC = g++
CFLAGS = -g -Wall

.PHONY: default all clean

default: $(TARGET)
all: default

OBJECTS = $(patsubst %.cpp, %.o, $(wildcard src/*.cpp)) $(patsubst %.c, %.o, $(wildcard src/*.c))
HEADERS = $(wildcard src/*.h)

%.o: %.cpp $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDE) -DUSE_KISS_FFT -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -Wall $(LDFLAGS) $(LIBS) -o $@

clean:
	-rm -f *.o
	-rm -f $(TARGET)
