CXXFLAGS = -g3 -Ofast -mavx2 -std=c++14 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wextra -pedantic \
		   -Wdisabled-optimization -Wctor-dtor-privacy -Wmissing-declarations \
		   -Woverloaded-virtual -Wshadow -Wno-unused -Winline

ifeq ($(shell uname), CYGWIN_NT-10.0)
  CXX = x86_64-w64-mingw32-g++
  LDLIBS = -lavformat -lavcodec -lavutil -lswresample -lswscale -lSDL2 -lSDL2_ttf -pthread -lz -liconv -lbcrypt -lbz2 -lws2_32 -lsecur32 -lole32
else
  CXX = g++
  LDLIBS = -lavformat -lavcodec -lavutil -lswscale -lSDL2 -lSDL2_ttf -pthread
endif

ifneq "$(wildcard /opt/homebrew)" ""
  CXXFLAGS += -I/opt/homebrew/include/
  LDLIBS += -L/opt/homebrew/lib/
else ifneq "$(wildcard /opt/local)" ""
  CXXFLAGS += -I/opt/local/include/
  LDLIBS += -L/opt/local/lib/
else
  CXXFLAGS += -I/usr/local/include/
  LDLIBS += -L/usr/local/lib/
endif

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
