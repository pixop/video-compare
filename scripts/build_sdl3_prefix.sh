#!/usr/bin/env bash
# Build pinned SDL3 + SDL3_ttf into a local prefix (no system install).
# Versions are read from windows_deps.mk at the repo root.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEPS_MK="${REPO_ROOT}/windows_deps.mk"

read_mk_var() {
  awk -v name="$1" -F' := ' '$1 == name { print $2; exit }' "$DEPS_MK"
}

SDL3_VERSION="$(read_mk_var SDL3_VERSION)"
SDL3_TTF_VERSION="$(read_mk_var SDL3_TTF_VERSION)"
: "${SDL3_VERSION:?SDL3_VERSION missing in windows_deps.mk}"
: "${SDL3_TTF_VERSION:?SDL3_TTF_VERSION missing in windows_deps.mk}"

SDL3_TAG="release-${SDL3_VERSION}"
TTF_TAG="release-${SDL3_TTF_VERSION}"

PREFIX="${1:-${SDL3_PREFIX:-/tmp/video-compare-sdl3-prefix}}"
BUILD_ROOT="${SDL3_BUILD_ROOT:-/tmp/video-compare-sdl3-build}/${SDL3_VERSION}-${SDL3_TTF_VERSION}"
JOBS="${JOBS:-2}"

if [[ -f "${PREFIX}/lib/pkgconfig/sdl3.pc" && -f "${PREFIX}/lib/pkgconfig/sdl3-ttf.pc" ]]; then
  if PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig" pkg-config --exact-version="${SDL3_VERSION}" sdl3 &&
     PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig" pkg-config --exact-version="${SDL3_TTF_VERSION}" sdl3-ttf; then
    echo "Pinned SDL3 prefix already present: ${PREFIX}"
    exit 0
  fi
  echo "SDL versions in ${PREFIX} do not match the pins. Use a fresh install prefix." >&2
  exit 1
fi

mkdir -p "${PREFIX}" "${BUILD_ROOT}"
cd "${BUILD_ROOT}"

if [[ ! -d SDL/.git ]]; then
  git clone --depth 1 --branch "${SDL3_TAG}" https://github.com/libsdl-org/SDL.git
fi
if [[ ! -d SDL_ttf/.git ]]; then
  git clone --depth 1 --branch "${TTF_TAG}" https://github.com/libsdl-org/SDL_ttf.git
fi

cmake -S SDL -B SDL/build \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL_SHARED=ON \
  -DSDL_STATIC=OFF \
  -DSDL_X11_XTEST=OFF \
  -DSDL_TESTS=OFF
cmake --build SDL/build -j"${JOBS}"
cmake --install SDL/build

cmake -S SDL_ttf -B SDL_ttf/build \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL3TTF_VENDORED=OFF \
  -DCMAKE_PREFIX_PATH="${PREFIX}"
cmake --build SDL_ttf/build -j"${JOBS}"
cmake --install SDL_ttf/build

echo "SDL3 prefix ready at ${PREFIX}"
PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig" pkg-config --modversion sdl3 sdl3-ttf