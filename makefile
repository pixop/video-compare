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

# Default: don't use pkg-config unless user explicitly enables it
# Usage: make USE_PKG_CONFIG=1
USE_PKG_CONFIG ?= 0

ifeq ($(USE_PKG_CONFIG),1)
  LDLIBS += $(shell pkg-config --libs libavformat libavcodec libavfilter libavutil libswscale libswresample sdl2 SDL2_ttf)
else
  LDLIBS += -lavformat -lavcodec -lavfilter -lavutil -lswscale -lswresample -lSDL2_ttf -lSDL2
endif

src = $(wildcard src/*.cpp)
obj = $(src:.cpp=.o)
dep = $(obj:.o=.d)
target = video-compare$(EXE)

test_src = $(wildcard tests/test_*.cpp)
test_obj = $(test_src:.cpp=.o)
test_dep = $(test_obj:.o=.d)
test_bin = $(patsubst %.cpp,%$(EXE),$(test_src))

integration_src = tests/integration_video_compare.cpp
integration_obj = $(integration_src:.cpp=.o)
integration_dep = $(integration_obj:.o=.d)
integration_bin = tests/integration_video_compare$(EXE)
integration_app_obj = $(filter-out src/main.o,$(obj))
integration_timeout_s ?= 45
stress_timeout_s ?= 35
# Override with STRESS_RUNS=100 make stress
stress_runs ?= $(if $(STRESS_RUNS),$(STRESS_RUNS),20)

all: $(target)

$(target): $(obj)
	$(CXX) -o $@ $^ $(LDLIBS)

-include $(dep) $(test_dep) $(integration_dep)

%.d: %.cpp
	@$(CXX) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@

test: $(target)
	./$(target) -w 800x docs/images/screenshot_1.jpg docs/images/screenshot_2.jpg

tests/test_%$(EXE): tests/test_%.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(TEST_LIBS)

tests/test_frame_metadata$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_playback_timing$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_playback_seek$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_playback_navigation$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_format_converter$(EXE): \
	src/format_converter.o src/frame_metadata.o src/ffmpeg.o src/side_aware_logger.o src/core_types.o
tests/test_format_converter$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_font_selection$(EXE): src/font_selection.o
tests/test_font_selection$(EXE): TEST_LIBS = $(LDLIBS)

$(integration_bin): $(integration_obj) $(integration_app_obj)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

.PHONY: check
check: $(test_bin)
	@set -e; \
	for test in $(test_bin); do \
		echo "Running $$test"; \
		./$$test; \
	done

.PHONY: integration
integration: $(integration_bin)
	@set -e; \
	if command -v timeout >/dev/null 2>&1 && timeout --version >/dev/null 2>&1; then \
		wrap="timeout $(integration_timeout_s)s"; \
	else \
		wrap=""; \
	fi; \
	media=$$(mktemp -d); \
	trap 'rm -rf "$$media"' EXIT; \
	echo "Generating integration fixtures in $$media"; \
	tests/generate_integration_media.sh "$$media"; \
	echo "Running $(integration_bin) baseline"; \
	$$wrap ./$(integration_bin) baseline "$$media/left_25.mp4" "$$media/right0_25.mp4"; \
	echo "Running $(integration_bin) event-injection"; \
	$$wrap ./$(integration_bin) event-injection "$$media/left_25.mp4" "$$media/right0_25.mp4" "$$media/right1_25.mp4"; \
	echo "Running $(integration_bin) seek"; \
	$$wrap ./$(integration_bin) seek "$$media/seek_left_25.mp4" "$$media/seek_right_25.mp4"; \
	echo "Running $(integration_bin) still-seek"; \
	$$wrap ./$(integration_bin) still-seek docs/images/screenshot_1.jpg docs/images/screenshot_2.jpg; \
	echo "Running $(integration_bin) sync-mismatch"; \
	$$wrap ./$(integration_bin) sync-mismatch "$$media/sync_left_25.mp4" "$$media/sync_right_30.mp4"; \
	echo "Running $(integration_bin) multi-right-sync"; \
	$$wrap ./$(integration_bin) multi-right-sync "$$media/multi_sync_left_30.mp4" "$$media/multi_sync_right0_25.mp4" "$$media/multi_sync_right1_25.mp4"; \
	echo "Running $(integration_bin) frame-navigation"; \
	$$wrap ./$(integration_bin) frame-navigation "$$media/seek_left_25.mp4" "$$media/seek_right_25.mp4"; \
	echo "Running $(integration_bin) buffer-forward-only"; \
	$$wrap ./$(integration_bin) buffer-forward-only "$$media/seek_left_25.mp4" "$$media/seek_right_25.mp4"; \
	echo "Running $(integration_bin) buffer-pingpong"; \
	$$wrap ./$(integration_bin) buffer-pingpong "$$media/seek_left_25.mp4" "$$media/seek_right_25.mp4"; \
	echo "Running $(integration_bin) crop-copy"; \
	$$wrap ./$(integration_bin) crop-copy "$$media/left_25.mp4" "$$media/right0_25.mp4" "$$media/right1_25.mp4" "$$media/right2_25.mp4"

.PHONY: stress
stress: $(integration_bin)
	@set -e; \
	if command -v timeout >/dev/null 2>&1 && timeout --version >/dev/null 2>&1; then \
		wrap="timeout $(stress_timeout_s)s"; \
	else \
		wrap=""; \
	fi; \
	runs="$(stress_runs)"; \
	media=$$(mktemp -d); \
	trap 'rm -rf "$$media"' EXIT; \
	echo "Generating stress fixtures in $$media"; \
	tests/generate_integration_media.sh "$$media" --stress; \
	i=1; \
	while [ "$$i" -le "$$runs" ]; do \
		echo "Running $(integration_bin) seek-burst-forward $$i/$$runs"; \
		STRESS_ITERATION=$$i $$wrap ./$(integration_bin) seek-burst-forward "$$media/stress_left_25.mp4" "$$media/stress_right_25.mp4"; \
		echo "Running $(integration_bin) seek-burst-mixed $$i/$$runs"; \
		STRESS_ITERATION=$$i $$wrap ./$(integration_bin) seek-burst-mixed "$$media/stress_left_25.mp4" "$$media/stress_right_25.mp4"; \
		i=$$((i + 1)); \
	done

.PHONY: check-one
check-one: tests/test_$(TEST)$(EXE)
	./tests/test_$(TEST)$(EXE)

.PHONY: clean
clean:
	# Also remove root-level objects/deps left by the pre-src/ layout.
	$(RM) $(obj) $(target) $(dep) $(test_obj) $(test_dep) $(test_bin) $(integration_obj) $(integration_dep) $(integration_bin) $(notdir $(obj)) $(notdir $(dep))

install: $(target)
	install -s $(target) $(BINDIR)
