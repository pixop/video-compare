CXX = g++
CXXFLAGS = -g3 -std=c++11 -D__STDC_CONSTANT_MACROS \
		   -Wall -Wextra -Wextra -pedantic \
		   -Wdisabled-optimization -Wctor-dtor-privacy -Wmissing-declarations \
		   -Woverloaded-virtual -Wshadow -Wno-unused -Winline
LDLIBS = -lavformat -lavcodec -lavutil -lswscale -lSDL2 -pthread

src = $(wildcard *.cpp)
obj = $(src:.cpp=.o)
dep = $(obj:.o=.d)
target = player

$(target): $(obj)
	$(CXX) -o $@ $^ $(LDLIBS)

-include $(dep)

%.d: %.cpp
	@$(CXX) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@

.PHONY: clean
clean:
	$(RM) $(obj) $(target) $(dep)
