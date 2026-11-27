# Included by the root makefile; paths are relative to the repository root.

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

test_artifacts = $(test_obj) $(test_dep) $(test_bin) $(integration_obj) $(integration_dep) $(integration_bin)

-include $(test_dep) $(integration_dep)

test: $(target)
	./$(target) -w 800x docs/images/screenshot_1.jpg docs/images/screenshot_2.jpg

tests/test_%$(EXE): tests/test_%.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(TEST_LIBS)

# Link the production DurationDeriver, not a test-local implementation.
tests/test_duration_deriver$(EXE): src/duration_deriver.o

tests/test_frame_metadata$(EXE): TEST_LIBS = $(LDLIBS)
tests/test_progress_dots$(EXE): TEST_LIBS = $(LDLIBS)
tests/test_split_texture_upload_cache$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_sdr_texture_render$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_queue$(EXE): TEST_LIBS = -pthread

tests/test_playback_timing$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_playback_seek$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_playback_navigation$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_playback_keyframe$(EXE): src/keyframe_index_probe.o
tests/test_playback_keyframe$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_demuxer_keyframe_seekable$(EXE): src/demuxer.o src/keyframe_index_probe.o src/keyframe_navigation.o \
	src/ffmpeg.o src/side_aware_logger.o src/core_types.o src/string_utils.o src/video_decoder.o src/duration_deriver.o
tests/test_demuxer_keyframe_seekable$(EXE): TEST_LIBS = $(LDLIBS)


tests/test_format_converter$(EXE): \
	src/format_converter.o src/frame_metadata.o src/ffmpeg.o src/side_aware_logger.o src/core_types.o
tests/test_format_converter$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_video_frame_buffer_pool$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_font_selection$(EXE): src/font_selection.o
tests/test_font_selection$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_hud_text_cache$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_video_filterer$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_tone_mapping$(EXE): src/tone_mapping.o
tests/test_tone_mapping$(EXE): TEST_LIBS = $(LDLIBS)

tests/test_scope_color$(EXE): src/scope_color.o
tests/test_scope_color$(EXE): TEST_LIBS = $(LDLIBS)

$(integration_bin): $(integration_obj) $(integration_app_obj)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

.PHONY: check
check: $(test_bin)
	@set -e; \
	for test in $(test_bin); do \
		echo "Running $$test"; \
		./$$test; \
	done
	./tests/test_split_texture_upload_cache$(EXE) --10-bpc
	@set -e; if command -v xvfb-run >/dev/null 2>&1; then \
		echo "Running tests/test_sdr_texture_render under Xvfb (GLX)"; \
		xvfb-run -a -s '-screen 0 1280x720x24 +extension GLX' ./tests/test_sdr_texture_render --accelerated; \
		echo "Running tests/test_split_texture_upload_cache --10-bpc --accelerated"; \
		xvfb-run -a -s '-screen 0 1280x720x24 +extension GLX' ./tests/test_split_texture_upload_cache --10-bpc --accelerated; \
	else \
		echo "SKIP accelerated SDR/10-bpc pixel tests (xvfb-run not available)"; \
		./tests/test_sdr_texture_render; \
	fi

.PHONY: integration
integration: $(integration_bin) tests/test_playback_keyframe$(EXE)
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
	echo "Running tests/test_playback_keyframe with indexed GOP fixture"; \
	./tests/test_playback_keyframe "$$media/seek_right_gop25.mp4"; \
	echo "Running tests/test_demuxer_keyframe_seekable (pipe nonseekable gate)"; \
	./tests/test_demuxer_keyframe_seekable "$$media/seek_right_gop25.mp4" "$$media/kf_ffv1_default.mkv" "$$media/kf_ffv1_gop1.mkv"; \
	echo "Running $(integration_bin) baseline"; \
	$$wrap ./$(integration_bin) baseline "$$media/left_25.mp4" "$$media/right0_25.mp4"; \
	echo "Running $(integration_bin) event-injection"; \
	$$wrap ./$(integration_bin) event-injection "$$media/left_25.mp4" "$$media/right0_25.mp4" "$$media/right1_25.mp4"; \
	echo "Running $(integration_bin) high-dpi-input"; \
	$$wrap ./$(integration_bin) high-dpi-input "$$media/left_25.mp4" "$$media/right0_25.mp4"; \
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
	if [ -f "$$media/kf_left_gop50.mp4" ] && [ -f "$$media/kf_right_gop50.mp4" ]; then \
		echo "Running $(integration_bin) keyframe-navigation"; \
		$$wrap ./$(integration_bin) keyframe-navigation "$$media/kf_left_gop50.mp4" "$$media/kf_right_gop50.mp4"; \
	else \
		echo "SKIP keyframe-navigation (libx264 GOP fixtures unavailable)"; \
	fi; \
	echo "Running $(integration_bin) keyframe-navigation-gop25"; \
	$$wrap ./$(integration_bin) keyframe-navigation-gop25 "$$media/seek_left_gop25.mp4" "$$media/seek_right_gop25.mp4"; \
	echo "Running $(integration_bin) keyframe-navigation-gop25 (nonzero video start)"; \
	KEYFRAME_NAV_EXPECT_START_MS=20 $$wrap ./$(integration_bin) keyframe-navigation-gop25 "$$media/kf_start_offset_left_50.mp4" "$$media/kf_start_offset_right_50.mp4"; \
	if [ -f "$$media/kf_asym_left_gop25.mp4" ] && [ -f "$$media/kf_asym_right_gop50.mp4" ]; then \
		echo "Running $(integration_bin) keyframe-navigation-asymmetric (left GOP25)"; \
		$$wrap ./$(integration_bin) keyframe-navigation-asymmetric "$$media/kf_asym_left_gop25.mp4" "$$media/kf_asym_right_gop50.mp4"; \
		echo "Running $(integration_bin) keyframe-navigation-asymmetric (reversed)"; \
		$$wrap ./$(integration_bin) keyframe-navigation-asymmetric "$$media/kf_asym_right_gop50.mp4" "$$media/kf_asym_left_gop25.mp4"; \
	else \
		echo "SKIP keyframe-navigation-asymmetric (libx264 GOP fixtures unavailable)"; \
	fi; \
	if [ -f "$$media/kf_union_left_48.mp4" ] && [ -f "$$media/kf_union_right_sparse_48.mp4" ]; then \
		echo "Running $(integration_bin) clipboard-seek (synthetic 48 fps)"; \
		$$wrap ./$(integration_bin) clipboard-seek "$$media/kf_union_left_48.mp4" "$$media/kf_union_right_sparse_48.mp4"; \
		echo "Running $(integration_bin) keyframe-navigation-union-time-shift (synthetic dense-left)"; \
		KEYFRAME_NAV_SHIFT_MS=40 KEYFRAME_NAV_WARMUP_D=0 KEYFRAME_NAV_STEPS=3 \
			$$wrap ./$(integration_bin) keyframe-navigation-union-time-shift \
			"$$media/kf_union_left_48.mp4" "$$media/kf_union_right_sparse_48.mp4"; \
		echo "Running $(integration_bin) keyframe-navigation-union-time-shift (synthetic sparse-left reversed)"; \
		KEYFRAME_NAV_SHIFT_MS=40 KEYFRAME_NAV_WARMUP_D=0 KEYFRAME_NAV_STEPS=3 \
			$$wrap ./$(integration_bin) keyframe-navigation-union-time-shift \
			"$$media/kf_union_right_sparse_48.mp4" "$$media/kf_union_left_48.mp4"; \
	else \
		echo "SKIP keyframe-navigation-union-time-shift (union GOP fixtures unavailable)"; \
	fi; \
	echo "Running $(integration_bin) buffer-forward-only"; \
	$$wrap ./$(integration_bin) buffer-forward-only "$$media/seek_left_25.mp4" "$$media/seek_right_25.mp4"; \
	echo "Running $(integration_bin) buffer-pingpong"; \
	$$wrap ./$(integration_bin) buffer-pingpong "$$media/seek_left_25.mp4" "$$media/seek_right_25.mp4"; \
	echo "Running $(integration_bin) crop-copy"; \
	$$wrap ./$(integration_bin) crop-copy "$$media/crop_left_320.mp4" "$$media/crop_right0_640.mp4" "$$media/crop_right1_320.mp4" "$$media/crop_right2_160.mp4"; \
	echo "Running $(integration_bin) interactive-crop"; \
	$$wrap ./$(integration_bin) interactive-crop "$$media/crop_left_320.mp4" "$$media/crop_right1_320.mp4"; \
	echo "Running $(integration_bin) dynamic-swap"; \
	$$wrap ./$(integration_bin) dynamic-swap "$$media/dynamic_left_43.mp4" "$$media/dynamic_right_169.mp4"

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
