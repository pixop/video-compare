CC = g++
CFLAGS = -g -Wall
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale `sdl-config --cflags --libs` -std=c++0x -D__STDC_CONSTANT_MACROS

TARGET = player

all: $(TARGET)

player: $(TARGET).o container.o
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).o container.o $(LDFLAGS)

container.o: container.cpp container.h
	$(CC) $(CFLAGS) -c container.cpp $(LDFLAGS)

$(TARGET).o: $(TARGET).cpp container.h queue.h
	$(CC) $(CFLAGS) -c $(TARGET).cpp $(LDFLAGS)

clean:
	rm -f *.o $(TARGET)

