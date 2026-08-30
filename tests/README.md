# Unit tests

Headless regression suite (also run by GitHub Actions on Linux, macOS, and Windows):

```sh
make check
```

One suite:

```sh
make check-one TEST=zoom_transform
```

`make test` is a separate GUI smoke (`video-compare` plus screenshots) and is not part of `check`. `make integration` runs the real `VideoCompare` compare loop under SDL dummy/software with ffmpeg-generated clips and the existing JPEG stills; it is also not part of `check`. Scenarios (`baseline`, `event-injection`, `seek`, `still-seek`, `sync-mismatch`, `multi-right-sync`, `frame-navigation`) each run in a fresh process. Any `tests/test_*.cpp` is picked up by `check` automatically. The integration source is `tests/integration_video_compare.cpp` so that wildcard does not include it. Header-only tests need no makefile change; tests that link production objects or FFmpeg still declare those extras in the makefile. Tests include production headers as `"foo.h"` via `-Isrc`. The integration target requires the `ffmpeg` CLI.

Covers effective DAR from width/height × SAR (invalid SAR → 1:1; non-positive dims → unavailable).

Conversion geometry covers native centering (including odd extra pixels on the right/bottom), oversized native content returning an empty rect, stretch fill-rect, crop mapping for inside/partial/padding-only selections, canvas-point mapping, and the stretch 1px-selection → min 2×2 crop regression.

Format converter tests cover native RGB24/RGB48LE placement (including odd horizontal offsets and canvas linesize larger than content width), src==canvas, mid-stream src growth that still fits, overflow errors, and stretch fill.

Zoom transform tests cover focal-point stability (the layout-space cursor or stack seam stays put) for Split / HStack / VStack, including letterboxed/pillarboxed windows, pan-then-zoom, keyboard 5/6/7 seam focal points, zoom-out, and repeated zoom steps. They use the production helpers also called by `Display::compute_relative_move_offset`, `update_move_offset`, `compute_zoom_rect`, `video_to_zoom_space`, and `window_to_video_position`. HStack/VStack use the single-frame center as the zoom transform origin while preserving layout-space focal points such as the cursor or stack seam. `./tests/test_zoom_transform --pre-fix` is a test-only replay of the old layout-center origin; it is not a production API.

Font selection tests cover forced embedded fonts, custom-file open/error formatting, UTF-8 well-formedness and glyph coverage, and Auto mode choosing Source Code Pro vs Sarasa from the two video labels.

## Manual / generated-stream checks

These exercise the real pipeline. Generate clips with ffmpeg as needed.

- Mid-stream `720×480 SAR 8:9` → `SAR 1:1`, with auto-filters on and with `--disable-auto-filters`: Dynamic letterboxes; OS window size stays put; Original is unchanged.
- `1920×1080` → `1280×720`: Dynamic does not change letterboxing (DAR remains 16:9).
- Mixed 4:3 left / 16:9 right: Dynamic follows the original left input; visual Swap (`S`) does not change the reference or letterboxing.
- Crop `1920×1080` → `1440×1080`: Dynamic becomes 4:3 after that frame is shown.
- `-x dynamic` startup: window is sized to the shared canvas; the first anamorphic reference frame letterboxes only (no extra `SDL_SetWindowSize`).
- Shift+S into Dynamic resizes the window once when windowed, or only relayouts `content_window_` in fullscreen.
- Dynamic + Content aspect lock + a frame DAR change must not call `SDL_SetWindowSize`.
- Zoom, pan, and split mapping still work after a Dynamic letterbox change.
- `--conversion-fit native` with mixed 4:3 / 16:9: each side is 1:1 with black bars; split/wipe still shares one canvas.
- `--conversion-size 1920x1440 --conversion-fit native`: canvas stays 1920x1440 after seek.
- Native + a later frame larger than the canvas: conversion errors instead of downscaling.
- `M` with native padding prints a one-time warning: "Objective metrics may include conversion-canvas padding in native fit mode."
