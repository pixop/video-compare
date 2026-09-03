# Unit tests

Headless regression suite (also run by GitHub Actions on Linux, macOS, and Windows):

```sh
make check
```

One suite:

```sh
make check-one TEST=zoom_transform
```

Other targets (not part of `check`):

- `make test` — GUI smoke: `video-compare` plus screenshots.
- `make integration` — real `VideoCompare` compare loop under SDL dummy/software. Uses ffmpeg-generated clips and the existing JPEG stills. Needs the `ffmpeg` CLI.
- `make stress` — same binary as integration. Runs `seek-burst-forward` and `seek-burst-mixed` on longer GOP-1 clips, 20 times each by default (`STRESS_RUNS=100 make stress`). Not part of `integration` either.

GitHub Actions runs `make`, `make check`, and `make integration` on Ubuntu 24.04, macOS 15 Intel, and Windows/MSYS2 UCRT64.

Integration scenarios (one fresh process each):

- `baseline`
- `event-injection`
- `seek`
- `still-seek`
- `sync-mismatch`
- `multi-right-sync`
- `frame-navigation`
- `buffer-forward-only`
- `buffer-pingpong`
- `crop-copy` — Shift+L/R mouse crop, Shift+O/I, Backspace undo, and right-video selection through the real Display → VideoCompare → VideoFilterer path. Asserts `crop=` in the Shift+X filter dump, plus `swapped` / `right` / visual-size ownership across Swap. Uses 320×180 / 640×360 / 160×90 rights so copy is normalized, not pixel-identical.
- `interactive-crop` — isolated post-scale Shift+L plus atomic Shift+B. Left is 320×180 with `scale=160:90`; right is 320×180 with `hflip`. Asserts the application `crop=` is in 320×180 crop-space (not the 160×90 dest), Backspace restores it, and Shift+B / unsupported visual-side crops change neither side.

Adding a unit test:

- `check` picks up `tests/test_*.cpp` automatically.
- Integration is `tests/integration_video_compare.cpp`, so that wildcard skips it.
- Header-only tests need no makefile change.
- Tests that link production objects or FFmpeg still declare those extras in the makefile.
- Production headers are included as `"foo.h"` via `-Isrc`.

What `check` covers:

- **Frame metadata** — effective DAR from width/height × SAR. Invalid SAR → 1:1. Non-positive dims → unavailable.
- **Conversion geometry** — native centering (odd extra pixels land on the right/bottom); oversized native content → empty rect; stretch fill-rect; crop mapping for inside / partial / padding-only selections; canvas-point mapping; stretch 1px selection → min 2×2 crop.
- **Format converter** — native RGB24 / RGB48LE placement (odd horizontal offsets; canvas linesize larger than content width); src==canvas; mid-stream src growth that still fits; overflow errors; stretch fill.
- **Zoom transform** — the layout-space cursor or stack seam stays put for Split / HStack / VStack (letterboxed and pillarboxed windows, pan-then-zoom, keyboard 5/6/7 seam focals, zoom-out, repeated steps). Uses the same helpers as `Display::compute_relative_move_offset`, `update_move_offset`, `compute_zoom_rect`, `video_to_zoom_space`, and `window_to_video_position`. HStack/VStack zoom from the single-frame center while keeping those layout-space focals. `./tests/test_zoom_transform --pre-fix` replays the old layout-center origin; it is not a production API.
- **Video filter state** — crop-only copy follows visual sides (Shift+O copies visual-left crop to the right-side video(s): all logical rights when unswapped, only the visual right / logical left after Swap; Shift+I copies visual-right crop to visual left), including after Swap; a pending copy or interactive crop keeps the Swap state and right index from commit/keypress, not a later live Display state; source/destination crop independence after copy; copy maps the source crop through normalized edges into each destination's crop-space size (same-size identity, exact 2x, downscale, non-integer rounding, different aspect, disabled source, in-bounds); a destination whose post-filter output is smaller still maps in crop-space, not that output size; `map_display_rect_to_crop_space` inverts dest selections through the current crop extent into crop-space (identity when dest equals the extent, post-scale 25–75%, existing crop plus post-scale, anisotropic resize, edge rounding, min 2×2, extent/crop-space clamps); Backspace undoes only the last crop operation (left-only, current-right-only, both, left-to-all-right-videos restoring distinct previous right states, right-to-left, LIFO sequence, empty no-op); undo uses the concrete sides from the original operation after later Swap or selection changes; Shift+L under Swap snapshots that right index; true no-ops do not consume an undo step; swapped Shift+O records only the visual right; partial Shift+O undo restores only the videos that changed; no-right copy is a no-op; `compose_filters` serialization used by Shift+X (`copy` when empty); linear filter-instance counting for crop-space walks; display-state quoting; Shift+X `swapped` / user-facing `right` mapping that does not exchange logical `filters_left/right`; selected-right index clamping.
- **Video filterer** — `crop_space_from_configured_chain` is the pre-crop insertion size on a real libavfilter graph: same value with crop enabled or disabled; a spatial post-filter changes dest but not crop-space; a spatial / transpose pre-filter does change crop-space; normalized copy uses that crop-space, not post-filter output. Post-crop interactive-crop classification (`scale`/`zscale`/`format`/`setsar` supported; `hflip`/`transpose`/`pad`/user `crop` unsupported; unknown same-size permitted; unknown dimension-changing and non-linear graphs refused) is exercised through the VideoFilterer-owned helper; app crop on/off does not change the result; pre-`transpose=clock` crop-space is 1080×1920.

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
