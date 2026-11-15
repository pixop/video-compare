#!/usr/bin/env bash
set -euo pipefail

app="${1:?usage: cli_smoke.sh PATH_TO_VIDEO_COMPARE}"
root="$(cd "$(dirname "$0")/.." && pwd)"
left="$root/docs/images/screenshot_1.jpg"
right="$root/docs/images/screenshot_2.jpg"

help="$({ "$app" --help 2>&1; } || true)"
if ! grep -q -- '-M' <<<"$help"; then
  echo "cli_smoke: --help missing -M" >&2
  exit 1
fi
if ! grep -q -- '--tone-map-algorithm' <<<"$help"; then
  echo "cli_smoke: --help missing --tone-map-algorithm" >&2
  exit 1
fi

err="$({ SDL_VIDEODRIVER=dummy "$app" -T rel -M :hable "$left" "$right" 2>&1; } || true)"
if ! grep -Fq 'got hable' <<<"$err"; then
  echo "cli_smoke: -M :hable not parsed (expected right-side hable validation error)" >&2
  echo "$err" >&2
  exit 1
fi

echo "cli_smoke: OK"