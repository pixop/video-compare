CXXFLAGS = -g3 -std=c++11 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wextra -pedantic \
		   -Wdisabled-optimization -Wctor-dtor-privacy -Wmissing-declarations \
		   -Woverloaded-virtual -Wshadow -Wno-unused -Winline
LDLIBS = -lavformat -lavcodec -lavutil -lswscale `sdl2-config --libs`

TARGET = player
OBJECTS = main.o player.o container.o display.o timer.o

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS) $(LDLIBS)

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
	$(RM) $(OBJECTS) $(TARGET)
