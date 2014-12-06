CXX = g++
CXXFLAGS = -g -Wall -Wextra -Wextra -pedantic -Wdisabled-optimization -Wctor-dtor-privacy -Wmissing-declarations -Woverloaded-virtual -Wshadow -Wno-unused -Winline -std=c++11 -D__STDC_CONSTANT_MACROS
LDLIBS = -lavformat -lavcodec -lavutil -lswscale `sdl2-config --libs`

TARGET = player

all: $(TARGET)

player: main.o player.o container.o display.o timer.o
	$(CXX) $(CXXFLAGS) -o $(TARGET) main.o container.o player.o display.o timer.o $(LDLIBS)

main.o: main.cpp player.h
	$(CXX) $(CXXFLAGS) -c main.cpp

player.o: player.cpp player.h container.h display.h queue.h timer.h
	$(CXX) $(CXXFLAGS) -c player.cpp

container.o: container.cpp container.h
	$(CXX) $(CXXFLAGS) -c container.cpp

display.o: display.cpp display.h
	$(CXX) $(CXXFLAGS) -c display.cpp

timer.o: timer.cpp timer.h
	$(CXX) $(CXXFLAGS) -c timer.cpp

clean:
	rm -f *.o $(TARGET)

