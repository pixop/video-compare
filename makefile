CXXFLAGS = -g3 -Ofast -std=c++14 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wno-deprecated -Wno-deprecated-declarations \
		   -Wdisabled-optimization -Wctor-dtor-privacy \
		   -Woverloaded-virtual -Wno-unused -Wno-missing-field-initializers

ifeq ($(findstring CYGWIN_NT-10.0, $(shell uname)), CYGWIN_NT-10.0)
  CXX = x86_64-w64-mingw32-g++
  CXXFLAGS += -Iffmpeg-6.1.1-full_build-shared/include/ -ISDL2-devel-2.28.5-mingw/SDL2-2.28.5/x86_64-w64-mingw32/include/
  LDLIBS = -Lffmpeg-6.1.1-full_build-shared/lib/ -LSDL2-devel-2.28.5-mingw/SDL2-2.28.5/x86_64-w64-mingw32/lib/
else
  CXX = g++
  LDLIBS = -pthread
endif

LDLIBS += -lavformat -lavcodec -lavfilter -lavutil -lswscale -lswresample -lSDL2 -lSDL2_ttf

ifneq "$(wildcard /opt/homebrew)" ""
  CXXFLAGS += -I/opt/homebrew/include/
  LDLIBS += -L/opt/homebrew/lib/
  BINDIR = /opt/homebrew/bin/
else ifneq "$(wildcard /opt/local)" ""
  CXXFLAGS += -I/opt/local/include/
  LDLIBS += -L/opt/local/lib/
  BINDIR = /opt/local/bin/
else
  CXXFLAGS += -I/usr/local/include/
  LDLIBS += -L/usr/local/lib/
  BINDIR = /usr/local/bin/
endif

ifneq "$(wildcard /usr/include/ffmpeg)" ""
  CXXFLAGS += -I/usr/include/ffmpeg
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
	./$(target) -w 800x screenshot_1.jpg screenshot_2.jpg

.PHONY: clean
clean:
	$(RM) $(obj) $(target) $(dep)

install: $(target)
	install -s video-compare $(BINDIR)
