TARGET = bin

CC 	= g++

DEFINES +=
CFLAGS = -Wall

#Include headers
INCLUDES = -I/home/renwei4/ffmpeg_build/include -I/usr/local/include

#Lib path
LIBS_PATH = -L/home/renwei4/ffmpeg_build/lib -L/usr/local/lib
LIBS = -lavformat -lavcodec -lavutil -lswscale -lswresample -lSDL2 -lm -lz -ldl -lpthread

#Include source files
SRCS = $(wildcard *.cpp)

OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET) : $(OBJS)
	$(CC) $(CFLAGS) $^ $(LIBS_PATH) $(LIBS) -o $@

%.o:%.cpp
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -c $^

clean:
	rm -f $(TARGET)
	rm -rf *.o






