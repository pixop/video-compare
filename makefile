CXX = g++
CC = $(CXX)
CXXFLAGS = -g3 -std=c++11 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wextra -pedantic \
		   -Wdisabled-optimization -Wctor-dtor-privacy -Wmissing-declarations \
		   -Woverloaded-virtual -Wshadow -Wno-unused -Winline
LDLIBS = -lavformat -lavcodec -lavutil -lswscale -lSDL2

TARGET = player
OBJECTS = main.o player.o container.o display.o timer.o

$(TARGET): $(OBJECTS)
main.o: main.cpp player.h
player.o: player.cpp player.h container.h display.h queue.h timer.h
container.o: container.cpp container.h
display.o: display.cpp display.h
timer.o: timer.cpp timer.h

.PHONY: clean
clean:
	$(RM) $(OBJECTS) $(TARGET)
