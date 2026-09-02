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
generate_clip "$out_dir/sync_left_25.mp4" 2 25
generate_clip "$out_dir/sync_right_30.mp4" 2 30
generate_clip "$out_dir/multi_sync_left_30.mp4" 3 30
generate_clip "$out_dir/multi_sync_right0_25.mp4" 3 25
generate_clip "$out_dir/multi_sync_right1_25.mp4" 3 25
generate_clip "$out_dir/crop_left_320.mp4" 1 25 320x180
generate_clip "$out_dir/crop_right0_640.mp4" 1 25 640x360
generate_clip "$out_dir/crop_right1_320.mp4" 1 25 320x180
generate_clip "$out_dir/crop_right2_160.mp4" 1 25 160x90
