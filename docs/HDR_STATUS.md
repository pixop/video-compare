# WIP: Native HDR status (SDL 3.4.20)

## What works (software-verified)

- Opt-in `--hdr` (default SDR path unchanged): PQ HDR10 and HLG **within one session** (same transfer on all inputs; no PQ+HLG mix).
- Decoder/filter tags: BT.2020 primaries, `bt2020nc`, PQ (`smpte2084`) or HLG (`arib-std-b67`); BT.2020 CL rejected.
- `FormatConverter` with `hdr_force_full_range_rgb_output`: limited-range YUV → **full-range RGB48** with preserved transfer metadata (`tests/test_hdr_yuv_fixture`).
- Per-input presentation to absolute linear nits, then BT.2020→709 and **platform scRGB content white** (80 nits Windows nominal; 100 nits macOS nominal) for `SDL_PIXELFORMAT_RGBA64_FLOAT` upload.
- BT.2100 HLG inverse OETF + OOTF at 1000 nits reference peak (`hdr_color`; 0.75 signal → ~203 nits in unit tests).
- Fail-closed runtime on Linux: native `--hdr` requires D3D11 or Metal renderer and HDR on renderer, window, and display (`hdr_native::require_viable_native_hdr_output`).
- SDR-referred overlay metrics (PSNR/SSIM/VMAF) blocked in native HDR mode.

## SDL 3.4.20 backend audit (source-only)

Set `SDL34_SOURCE` to an SDL 3.4.20 source checkout when running `make check` to audit the D3D11 and Metal shader sources for PQ decode and `mat2020to709`, and the renderer sources for floating-point texture support. The audit skips when no source checkout is specified.

**Hardware unverified:** No Windows D3D11 or macOS Metal HDR display validation in this Linux CI environment.

## Remaining blockers

| Item | Notes |
|------|--------|
| Linux interactive `--hdr` | Expected startup failure (no D3D11/Metal + HDR display). |
| Mixed PQ + HLG compare | Rejected at qualification. |
| Per-input mixed reference peaks (HLG) | Single `kHlgReferencePeakNits` (1000) for OOTF; no per-file override yet. |
| HDR objective metrics | Intentionally unavailable; not HDR-referred. |
| Visual E2E on real HDR panels | Requires Win/mac hardware. |