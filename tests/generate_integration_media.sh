#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 OUT_DIR [--stress]" >&2
  exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "error: ffmpeg CLI is required to generate integration fixtures" >&2
  exit 1
fi

out_dir=$1
mkdir -p "$out_dir"

# MPEG-4 with GOP size 1 keeps every frame intra-coded so later seek
# scenarios can reuse the same generator without closed-GOP surprises.
# Separate output paths avoid VideoCompare single-decoder mode.
generate_clip() {
  dest=$1
  duration=$2
  rate=${3:-25}
  size=${4:-320x180}
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc=size=${size}:rate=${rate}:duration=${duration}" \
    -pix_fmt yuv420p -c:v mpeg4 -q:v 5 -g 1 \
    "$dest"
}

if [ "${2:-}" = "--stress" ]; then
  generate_clip "$out_dir/stress_left_25.mp4" 20
  generate_clip "$out_dir/stress_right_25.mp4" 20
  exit 0
fi

generate_clip "$out_dir/left_25.mp4" 1
generate_clip "$out_dir/right0_25.mp4" 1
generate_clip "$out_dir/right1_25.mp4" 1
generate_clip "$out_dir/right2_25.mp4" 1
generate_clip "$out_dir/seek_left_25.mp4" 4
generate_clip "$out_dir/seek_right_25.mp4" 4

# Matroska loads its Cues lazily; exercise both sparse and all-keyframe FFV1 indexes.
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc=size=160x90:rate=25:duration=2" -c:v ffv1 "$out_dir/kf_ffv1_default.mkv"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc=size=160x90:rate=25:duration=2" -c:v ffv1 -g 1 "$out_dir/kf_ffv1_gop1.mkv"

# MPEG-4 GOP 25 with 2 B-frames per GOP (identical left/right for integration).
generate_gop25_clip() {
  dest=$1
  duration=$2
  rate=${3:-25}
  size=${4:-320x180}
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc=size=${size}:rate=${rate}:duration=${duration}" \
    -pix_fmt yuv420p -c:v mpeg4 -q:v 5 -bf 2 -g 25 \
    "$dest"
}
generate_gop25_clip "$out_dir/seek_left_gop25.mp4" 4
generate_gop25_clip "$out_dir/seek_right_gop25.mp4" 4

generate_gop_clip() {
  dest=$1
  duration=$2
  gop=${3:-50}
  rate=${4:-25}
  size=${5:-320x180}
  if ! ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc=size=${size}:rate=${rate}:duration=${duration}" \
    -pix_fmt yuv420p -c:v libx264 -preset ultrafast -g "${gop}" -keyint_min "${gop}" -bf 2 \
    "$dest"; then
    echo "warn: libx264 GOP fixture ${dest} not generated (encoder missing?)" >&2
  fi
}

generate_gop_clip "$out_dir/kf_left_gop50.mp4" 4 50
generate_gop_clip "$out_dir/kf_right_gop50.mp4" 4 50
generate_gop_clip "$out_dir/kf_asym_left_gop25.mp4" 4 25
generate_gop_clip "$out_dir/kf_asym_right_gop50.mp4" 4 50

# Union shift regression: left GOP 50 @ 48 fps (~1.041667 s keys, B-frames) + right keys only at 0 and 5.208333 s.
if ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc=size=320x180:rate=48:duration=10" \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -g 50 -keyint_min 50 -sc_threshold 0 -bf 2 \
  "$out_dir/kf_union_left_48.mp4"; then
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc=size=320x180:rate=48:duration=10" \
    -pix_fmt yuv420p -c:v libx264 -preset ultrafast -g 480 -keyint_min 480 -sc_threshold 0 -bf 2 \
    -force_key_frames "0,5.208333" \
    "$out_dir/kf_union_right_sparse_48.mp4" || true
fi
generate_clip "$out_dir/sync_left_25.mp4" 2 25
generate_clip "$out_dir/sync_right_30.mp4" 2 30
generate_clip "$out_dir/multi_sync_left_30.mp4" 3 30
generate_clip "$out_dir/multi_sync_right0_25.mp4" 3 25
generate_clip "$out_dir/multi_sync_right1_25.mp4" 3 25
generate_clip "$out_dir/crop_left_320.mp4" 1 25 320x180
generate_clip "$out_dir/crop_right0_640.mp4" 1 25 640x360
generate_clip "$out_dir/crop_right1_320.mp4" 1 25 320x180
generate_clip "$out_dir/crop_right2_160.mp4" 1 25 160x90
generate_clip "$out_dir/dynamic_left_43.mp4" 1 25 320x240
generate_clip "$out_dir/dynamic_right_169.mp4" 1 25 640x360
