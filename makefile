CXXFLAGS = -g3 -Ofast -std=c++14 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wno-deprecated -Wno-deprecated-declarations \
		   -Wdisabled-optimization -Wctor-dtor-privacy \
		   -Woverloaded-virtual -Wno-unused -Wno-missing-field-initializers

ifeq ($(findstring CYGWIN_NT-10.0, $(shell uname)), CYGWIN_NT-10.0)
  FFMPEG_VERSION = 7.1.1-full_build-shared
  SDL3_VERSION = 3.2.14
  SDL3_TTF_VERSION = 3.2.2

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

LDLIBS += -lavformat -lavcodec -lavfilter -lavutil -lswscale -lswresample -lSDL3 -lSDL3_ttf

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
