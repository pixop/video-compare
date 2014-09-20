CC = g++
CFLAGS = -g -Wall -Wextra -Wextra
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale `sdl2-config --cflags --libs` -std=c++11 -D__STDC_CONSTANT_MACROS

TARGET = player

all: $(TARGET)

player: main.o player.o container.o display.o timer.o
	$(CC) $(CFLAGS) -o $(TARGET) main.o container.o player.o display.o timer.o $(LDFLAGS)

main.o: main.cpp player.h
	$(CC) $(CFLAGS) -c main.cpp $(LDFLAGS)

player.o: player.cpp player.h container.h display.h queue.h timer.h
	$(CC) $(CFLAGS) -c player.cpp $(LDFLAGS)

container.o: container.cpp container.h
	$(CC) $(CFLAGS) -c container.cpp $(LDFLAGS)

display.o: display.cpp display.h
	$(CC) $(CFLAGS) -c display.cpp $(LDFLAGS)

timer.o: timer.cpp timer.h
	$(CC) $(CFLAGS) -c timer.cpp $(LDFLAGS)

clean:
	rm -f *.o $(TARGET)

