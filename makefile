CXXFLAGS = -g3 -Ofast -std=c++14 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wno-deprecated -Wno-deprecated-declarations \
		   -Wdisabled-optimization -Wctor-dtor-privacy \
		   -Woverloaded-virtual -Wno-unused -Wno-missing-field-initializers
CXXFLAGS += -Isrc -Ithird_party

EXE =

ifneq ($(filter MINGW%,$(shell uname)),)
  EXE = .exe
  ifeq ($(MSYSTEM),UCRT64)
    # MSYS2 UCRT64 toolchain packages (CI integration). Headers/libs are on the
    # default g++ search path; do not pull the Gyan/MinGW download layout.
    CXX = g++
    LDLIBS = -pthread
  else
    include windows_deps.mk
    FFMPEG_VERSION = $(GYAN_FFMPEG_VERSION)-$(GYAN_FFMPEG_VARIANT)

    FFMPEG_PATH = ffmpeg-$(FFMPEG_VERSION)
    SDL3_PATH = SDL3-devel-$(SDL3_VERSION)-mingw/SDL3-$(SDL3_VERSION)/x86_64-w64-mingw32
    SDL3_TTF_PATH = SDL3_ttf-devel-$(SDL3_TTF_VERSION)-mingw/SDL3_ttf-$(SDL3_TTF_VERSION)/x86_64-w64-mingw32

    CXX = x86_64-w64-mingw32-g++
    CXXFLAGS += -I$(FFMPEG_PATH)/include/ \
                -I$(SDL3_PATH)/include/ \
                -I$(SDL3_TTF_PATH)/include/
    LDLIBS += -L$(FFMPEG_PATH)/lib/ \
              -L$(SDL3_PATH)/lib/ \
              -L$(SDL3_TTF_PATH)/lib/
  endif
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

# Optional local SDL3 prefix (e.g. /tmp/video-compare-sdl3-prefix from a source build).
# Usage: make SDL3_PREFIX=/path/to/prefix
SDL3_PREFIX ?=
ifneq ($(SDL3_PREFIX),)
  CXXFLAGS += -I$(SDL3_PREFIX)/include
  LDLIBS += -L$(SDL3_PREFIX)/lib -Wl,-rpath,$(SDL3_PREFIX)/lib
  export PKG_CONFIG_PATH := $(SDL3_PREFIX)/lib/pkgconfig$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH),)
endif

# Default: don't use pkg-config unless user explicitly enables it
# Usage: make USE_PKG_CONFIG=1
USE_PKG_CONFIG ?= 0

ifeq ($(USE_PKG_CONFIG),1)
  SDL3_CFLAGS := $(shell pkg-config --cflags sdl3 sdl3-ttf 2>/dev/null)
  SDL3_LIBS := $(shell pkg-config --libs sdl3 sdl3-ttf 2>/dev/null)
  ifeq ($(SDL3_LIBS),)
    $(error SDL3 pkg-config modules sdl3 and sdl3-ttf are required; set SDL3_PREFIX or install SDL3 development packages)
  endif
  CXXFLAGS += $(SDL3_CFLAGS)
  LDLIBS += $(shell pkg-config --libs libavformat libavcodec libavfilter libavutil libswscale libswresample) $(SDL3_LIBS)
else
  LDLIBS += -lavformat -lavcodec -lavfilter -lavutil -lswscale -lswresample -lSDL3_ttf -lSDL3
endif

src = $(wildcard src/*.cpp)
obj = $(src:.cpp=.o)
dep = $(obj:.o=.d)
target = video-compare$(EXE)

all: $(target)

$(target): $(obj)
	$(CXX) -o $@ $^ $(LDLIBS)

-include $(dep)

%.d: %.cpp
	@$(CXX) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@

include tests/tests.mk

.PHONY: clean
clean:
	# Also remove root-level objects/deps left by the pre-src/ layout.
	$(RM) $(obj) $(target) $(dep) $(test_artifacts) $(notdir $(obj)) $(notdir $(dep))

install: $(target)
	install -s $(target) $(BINDIR)
