CXX = g++
#CXX = x86_64-w64-mingw32-g++
CXXFLAGS = -g3 -Ofast -mavx2 -std=c++14 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wextra -pedantic \
		   -Wdisabled-optimization -Wctor-dtor-privacy -Wmissing-declarations \
		   -Woverloaded-virtual -Wshadow -Wno-unused -Winline \
		   -I/opt/local/include/
LDLIBS = -L/opt/local/lib/ -lavformat -lavcodec -lavutil -lswscale -lSDL2 -lSDL2_ttf -pthread
#LDLIBS = -L/usr/local/lib/ -lavformat -lavcodec -lavutil -lswresample -lswscale -lSDL2 -lSDL2_ttf -pthread -lz -liconv -lbcrypt -lbz2 -lws2_32 -lsecur32 -lole32

src = $(wildcard *.cpp)
obj = $(src:.cpp=.o)
dep = $(obj:.o=.d)
target = video-compare

all: $(target)

$(target): $(obj)
	$(CXX) -o $@ $^ $(LDLIBS)

-include $(dep)

%.d: %.cpp
	@$(CXX) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@

test: $(target)
	./$(target) test.mkv

.PHONY: clean
clean:
	$(RM) $(obj) $(target) $(dep)
