CC = g++
CFLAGS = -g -Wall
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale `sdl-config --cflags --libs` -std=c++0x -D__STDC_CONSTANT_MACROS

TARGET = player

all: $(TARGET)

player: main.o player.o container.o display.o
	$(CC) $(CFLAGS) -o $(TARGET) main.o container.o player.o display.o $(LDFLAGS)

main.o: main.cpp player.h
	$(CC) $(CFLAGS) -c main.cpp $(LDFLAGS)

player.o: player.cpp player.h container.h display.h
	$(CC) $(CFLAGS) -c player.cpp $(LDFLAGS)

container.o: container.cpp container.h
	$(CC) $(CFLAGS) -c container.cpp $(LDFLAGS)

display.o: display.cpp display.h
	$(CC) $(CFLAGS) -c display.cpp $(LDFLAGS)

clean:
	rm -f *.o $(TARGET)

