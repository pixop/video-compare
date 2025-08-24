CXXFLAGS = -g3 -Ofast -std=c++14 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wno-deprecated -Wno-deprecated-declarations \
		   -Wdisabled-optimization -Wctor-dtor-privacy \
		   -Woverloaded-virtual -Wno-unused -Wno-missing-field-initializers

ifeq ($(findstring CYGWIN_NT-10.0, $(shell uname)), CYGWIN_NT-10.0)
  FFMPEG_VERSION = 8.0-full_build-shared
  SDL2_VERSION = 2.32.8
  SDL2_TTF_VERSION = 2.24.0

  FFMPEG_PATH = ffmpeg-$(FFMPEG_VERSION)
  SDL2_PATH = SDL2-devel-$(SDL2_VERSION)-mingw/SDL2-$(SDL2_VERSION)/x86_64-w64-mingw32
  SDL2_TTF_PATH = SDL2_ttf-devel-$(SDL2_TTF_VERSION)-mingw/SDL2_ttf-$(SDL2_TTF_VERSION)/x86_64-w64-mingw32

  CXX = x86_64-w64-mingw32-g++
  CXXFLAGS += -I$(FFMPEG_PATH)/include/ \
              -I$(SDL2_PATH)/include/ \
              -I$(SDL2_PATH)/include/SDL2/ \
              -I$(SDL2_TTF_PATH)/include/
  LDLIBS += -L$(FFMPEG_PATH)/lib/ \
            -L$(SDL2_PATH)/lib/ \
            -L$(SDL2_TTF_PATH)/lib/
else
  CXX = g++
  LDLIBS = -pthread
endif

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

# Default: don't use pkg-config unless user explicitly enables it
# Usage: make USE_PKG_CONFIG=1
USE_PKG_CONFIG ?= 0

ifeq ($(USE_PKG_CONFIG),1)
  LDLIBS += $(shell pkg-config --libs libavformat libavcodec libavfilter libavutil libswscale libswresample sdl2 SDL2_ttf)
else
  LDLIBS += -lavformat -lavcodec -lavfilter -lavutil -lswscale -lswresample -lSDL2_ttf -lSDL2
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
