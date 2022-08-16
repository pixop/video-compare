CXXFLAGS = -g3 -Ofast -mavx -std=c++14 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wno-deprecated -Wno-deprecated-declarations \
		   -Wdisabled-optimization -Wctor-dtor-privacy \
		   -Woverloaded-virtual -Wno-unused -Wno-missing-field-initializers

ifeq ($(shell uname), CYGWIN_NT-10.0)
  CXX = x86_64-w64-mingw32-g++
  CXXFLAGS += -Iffmpeg-5.0.1-full_build-shared/include/
  LDLIBS = -Lffmpeg-5.0.1-full_build-shared/lib/ -lavformat -lavcodec -lavutil -lswresample -lswscale -lSDL2 -lSDL2_ttf
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
	./$(target) -w 1280x720 screenshot_1.jpg screenshot_2.jpg

.PHONY: clean
clean:
	$(RM) $(obj) $(target) $(dep)
