#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
DEPS_MK="${SCRIPT_DIR}/windows_deps.mk"

trim_value() {
    local value="$1"
    value="${value//$'\r'/}"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "$value"
}

read_mk_var() {
    local name="$1"
    local value

    if [[ ! -f "$DEPS_MK" ]]; then
        echo "Missing dependency version file: $DEPS_MK" >&2
        return 1
    fi

    value="$(awk -v name="$name" -F' := ' '$1 == name { print $2; exit }' "$DEPS_MK")"
    value="$(trim_value "$value")"

    if [[ -z "$value" ]]; then
        echo "Required variable ${name} is missing or empty in ${DEPS_MK}" >&2
        return 1
    fi

    printf '%s' "$value"
}

validate_sha256_config() {
    local name="$1"
    local value="$2"

    if [[ ! "$value" =~ ^[0-9a-fA-F]{64}$ ]]; then
        echo "Invalid configured SHA-256 checksum (${name}) in ${DEPS_MK}" >&2
        return 1
    fi
}

require_ffmpeg_deps() {
    GYAN_FFMPEG_VERSION="$(read_mk_var GYAN_FFMPEG_VERSION)"
    GYAN_FFMPEG_VARIANT="$(read_mk_var GYAN_FFMPEG_VARIANT)"
    GYAN_FFMPEG_ARCHIVE_SHA256="$(read_mk_var GYAN_FFMPEG_ARCHIVE_SHA256)"
    validate_sha256_config "GYAN_FFMPEG_ARCHIVE_SHA256" "$GYAN_FFMPEG_ARCHIVE_SHA256"
}

require_sdl2_deps() {
    SDL2_VERSION="$(read_mk_var SDL2_VERSION)"
    SDL2_WIN32_X64_ZIP_SHA256="$(read_mk_var SDL2_WIN32_X64_ZIP_SHA256)"
    SDL2_DEVEL_MINGW_TAR_SHA256="$(read_mk_var SDL2_DEVEL_MINGW_TAR_SHA256)"
    validate_sha256_config "SDL2_WIN32_X64_ZIP_SHA256" "$SDL2_WIN32_X64_ZIP_SHA256"
    validate_sha256_config "SDL2_DEVEL_MINGW_TAR_SHA256" "$SDL2_DEVEL_MINGW_TAR_SHA256"
}

require_sdl2_ttf_deps() {
    SDL2_TTF_VERSION="$(read_mk_var SDL2_TTF_VERSION)"
    SDL2_TTF_WIN32_X64_ZIP_SHA256="$(read_mk_var SDL2_TTF_WIN32_X64_ZIP_SHA256)"
    SDL2_TTF_DEVEL_MINGW_TAR_SHA256="$(read_mk_var SDL2_TTF_DEVEL_MINGW_TAR_SHA256)"
    validate_sha256_config "SDL2_TTF_WIN32_X64_ZIP_SHA256" "$SDL2_TTF_WIN32_X64_ZIP_SHA256"
    validate_sha256_config "SDL2_TTF_DEVEL_MINGW_TAR_SHA256" "$SDL2_TTF_DEVEL_MINGW_TAR_SHA256"
}

gyan_ffmpeg_archive_name() {
    echo "ffmpeg-${GYAN_FFMPEG_VERSION}-${GYAN_FFMPEG_VARIANT}.7z"
}

gyan_ffmpeg_extract_dir() {
    echo "ffmpeg-${GYAN_FFMPEG_VERSION}-${GYAN_FFMPEG_VARIANT}"
}

remove_tree_if_expected() {
    local directory="$1"
    local label="$2"

    directory="$(trim_value "$directory")"
    if [[ -z "$directory" || "$directory" == "/" || "$directory" == "." ]]; then
        echo "Refusing to remove unsafe ${label} directory: '${directory}'" >&2
        return 1
    fi

    if [[ -e "$directory" ]]; then
        rm -rf -- "$directory"
    fi
}

verify_archive_sha256() {
    local archive_path="$1"
    local expected_sha256="$2"

    if [[ ! "$expected_sha256" =~ ^[0-9a-fA-F]{64}$ ]]; then
        echo "Invalid configured SHA-256 checksum for archive: ${archive_path}" >&2
        return 1
    fi

    expected_sha256="$(printf '%s' "$expected_sha256" | tr '[:upper:]' '[:lower:]')"
    local actual_sha256
    actual_sha256="$(sha256sum "$archive_path" | awk '{print $1}')"
    actual_sha256="$(printf '%s' "$actual_sha256" | tr '[:upper:]' '[:lower:]')"

    if [[ "$actual_sha256" != "$expected_sha256" ]]; then
        echo "SHA-256 verification failed for archive: ${archive_path}" >&2
        echo "Expected: ${expected_sha256}" >&2
        echo "Actual:   ${actual_sha256}" >&2
        return 1
    fi
}

github_wget() {
    local url="$1"
    local output=""
    local status=0
    local xtrace_was_on=0

    case "$-" in
        *x*) xtrace_was_on=1 ;;
    esac

    if [[ "$xtrace_was_on" -eq 1 ]]; then
        set +x
    fi

    if [[ -n "${GITHUB_TOKEN:-}" ]]; then
        if output="$(wget -qO- --header="Authorization: Bearer ${GITHUB_TOKEN}" "$url")"; then
            status=0
        else
            status=$?
            output=""
        fi
    else
        if output="$(wget -qO- "$url")"; then
            status=0
        else
            status=$?
            output=""
        fi
    fi

    if [[ "$xtrace_was_on" -eq 1 ]]; then
        set -x
    fi

    if [[ "$status" -ne 0 ]]; then
        return "$status"
    fi

    printf '%s' "$output"
    return 0
}

fetch_gyan_release_json() {
    local api_url="https://api.github.com/repos/GyanD/codexffmpeg/releases/tags/${GYAN_FFMPEG_VERSION}"
    local release_json

    if ! release_json="$(github_wget "$api_url")"; then
        echo "Failed to fetch Gyan release metadata for tag ${GYAN_FFMPEG_VERSION}." >&2
        return 1
    fi

    if [[ -z "$release_json" ]]; then
        echo "Empty response when fetching Gyan release metadata for tag ${GYAN_FFMPEG_VERSION}." >&2
        return 1
    fi

    if ! grep -Fq "\"tag_name\": \"${GYAN_FFMPEG_VERSION}\"" <<< "$release_json"; then
        echo "Gyan release tag ${GYAN_FFMPEG_VERSION} was not found on GitHub." >&2
        return 1
    fi

    printf '%s' "$release_json"
}

fetch_gyan_release_commit() {
    local api_url="https://api.github.com/repos/GyanD/codexffmpeg/commits/${GYAN_FFMPEG_VERSION}"
    local commit_json commit_sha

    if ! commit_json="$(github_wget "$api_url")"; then
        echo "Failed to fetch Gyan release commit for ${GYAN_FFMPEG_VERSION}." >&2
        return 1
    fi

    if [[ -z "$commit_json" ]]; then
        echo "Empty response when resolving Gyan release commit for ${GYAN_FFMPEG_VERSION}." >&2
        return 1
    fi

    commit_sha="$(awk -F'"' '$2 == "sha" { print $4; exit }' <<< "$commit_json")"
    if [[ ! "$commit_sha" =~ ^[0-9a-f]{40}$ ]]; then
        echo "Failed to resolve Gyan release commit for ${GYAN_FFMPEG_VERSION}." >&2
        return 1
    fi

    printf '%s' "$commit_sha"
}

parse_ffmpeg_source_urls() {
    local release_json="$1"
    local url

    FFMPEG_SOURCE_URLS=()
    while IFS= read -r url; do
        [[ -n "$url" ]] && FFMPEG_SOURCE_URLS+=("$url")
    done < <(grep -oE 'https://github.com/FFmpeg/FFmpeg/commit/[0-9a-f]+' <<< "$release_json" | sort -u)
}

parse_ffmpeg_source_url() {
    local release_json="$1"

    parse_ffmpeg_source_urls "$release_json"
    if [[ ${#FFMPEG_SOURCE_URLS[@]} -eq 0 ]]; then
        echo "Failed to parse FFmpeg upstream source revision from Gyan release ${GYAN_FFMPEG_VERSION}." >&2
        return 1
    fi
    if [[ ${#FFMPEG_SOURCE_URLS[@]} -gt 1 ]]; then
        echo "Expected exactly one unique FFmpeg source revision in Gyan release ${GYAN_FFMPEG_VERSION}, found ${#FFMPEG_SOURCE_URLS[@]}." >&2
        return 1
    fi

    printf '%s' "${FFMPEG_SOURCE_URLS[0]}"
}

verify_gyan_shared_archive_asset() {
    local release_json="$1"
    local archive_name="$2"

    if ! grep -Fq "\"name\": \"${archive_name}\"" <<< "$release_json"; then
        echo "Gyan release ${GYAN_FFMPEG_VERSION} does not publish asset ${archive_name}." >&2
        return 1
    fi
}

parse_ffmpeg_version_token() {
    local line="$1"

    if [[ "$line" =~ ffmpeg[[:space:]]version[[:space:]]+([^[:space:]]+) ]]; then
        printf '%s' "${BASH_REMATCH[1]}"
        return 0
    fi

    return 1
}

ffmpeg_version_matches() {
    local actual="$1"
    local expected="$2"

    [[ "$actual" == "$expected" || "$actual" == "${expected}-"* ]]
}

generate_ffmpeg_source_manifest() {
    local extracted_dir="$1"
    local archive_name="$2"
    local release_json source_url source_commit ffmpeg_version_line actual_version manifest_path
    local gyan_release_commit ffmpeg_source_archive gyan_release_archive gyan_build_scripts_repo

    if ! release_json="$(fetch_gyan_release_json)"; then
        return 1
    fi

    if ! source_url="$(parse_ffmpeg_source_url "$release_json")"; then
        return 1
    fi

    source_commit="${source_url##*/}"
    verify_gyan_shared_archive_asset "$release_json" "$archive_name"

    if ! gyan_release_commit="$(fetch_gyan_release_commit)"; then
        return 1
    fi

    if [[ ! -d "$extracted_dir/bin" ]]; then
        echo "Expected FFmpeg bin directory missing in ${extracted_dir}." >&2
        return 1
    fi

    if ! read -r ffmpeg_version_line < <("$extracted_dir/bin/ffmpeg.exe" -version 2>/dev/null); then
        echo "Failed to read FFmpeg version from ${extracted_dir}/bin/ffmpeg.exe." >&2
        return 1
    fi

    if ! actual_version="$(parse_ffmpeg_version_token "$ffmpeg_version_line")"; then
        echo "Failed to parse FFmpeg version token from: ${ffmpeg_version_line}" >&2
        return 1
    fi

    if ! ffmpeg_version_matches "$actual_version" "$GYAN_FFMPEG_VERSION"; then
        echo "Bundled FFmpeg version mismatch." >&2
        echo "Expected version prefix: ${GYAN_FFMPEG_VERSION}" >&2
        echo "Actual version token:    ${actual_version}" >&2
        echo "Actual version line:     ${ffmpeg_version_line}" >&2
        return 1
    fi

    ffmpeg_source_archive="https://github.com/FFmpeg/FFmpeg/archive/${source_commit}.tar.gz"
    gyan_release_archive="https://github.com/GyanD/codexffmpeg/archive/refs/tags/${GYAN_FFMPEG_VERSION}.tar.gz"
    gyan_build_scripts_repo="https://github.com/GyanD/media-autobuild_suite"

    manifest_path="${SCRIPT_DIR}/licenses/FFMPEG-SOURCE.txt"
    mkdir -p "$(dirname "$manifest_path")"

    cat > "$manifest_path" <<EOF
Source and build information for the bundled FFmpeg libraries
==============================================================

This file records source and build information for the Gyan FFmpeg
shared libraries bundled with this Windows artifact. It is not itself
the complete corresponding source for the full Gyan build, which also
depends on additional libraries and build scripts.

Gyan release version:      ${GYAN_FFMPEG_VERSION}
Gyan build variant:        ${GYAN_FFMPEG_VARIANT} (GPL-enabled full shared build)
FFmpeg version:            ${GYAN_FFMPEG_VERSION}
FFmpeg upstream revision:  ${source_commit}
FFmpeg upstream commit:    ${source_url}
FFmpeg upstream archive:   ${ffmpeg_source_archive}

Gyan release tag:          ${GYAN_FFMPEG_VERSION}
Gyan release page:         https://github.com/GyanD/codexffmpeg/releases/tag/${GYAN_FFMPEG_VERSION}
Gyan release repository:   https://github.com/GyanD/codexffmpeg
Gyan release archive:      ${gyan_release_archive}
Gyan release commit:       ${gyan_release_commit}
Gyan builds index:         https://www.gyan.dev/ffmpeg/builds/
Gyan build-script repo:    ${gyan_build_scripts_repo}

Bundled shared-build archive:
  ${archive_name}

Archive download URL:
  https://github.com/GyanD/codexffmpeg/releases/download/${GYAN_FFMPEG_VERSION}/${archive_name}

Archive SHA-256:
  ${GYAN_FFMPEG_ARCHIVE_SHA256}

Extracted directory:
  ${extracted_dir}/

Bundled FFmpeg version string:
  ${ffmpeg_version_line}
EOF

    echo "Wrote ${manifest_path}"
}

download_ffmpeg() {
    require_ffmpeg_deps

    local archive_name extract_dir download_url extracted_path

    archive_name="$(gyan_ffmpeg_archive_name)"
    extract_dir="$(gyan_ffmpeg_extract_dir)"
    download_url="https://github.com/GyanD/codexffmpeg/releases/download/${GYAN_FFMPEG_VERSION}/${archive_name}"

    echo "Downloading pinned Gyan FFmpeg build ${archive_name}..."
    wget "$download_url" -O "$archive_name"

    echo "Verifying SHA-256 for ${archive_name}..."
    verify_archive_sha256 "$archive_name" "$GYAN_FFMPEG_ARCHIVE_SHA256"

    echo "Removing any previous extraction directory ${extract_dir}..."
    remove_tree_if_expected "$extract_dir" "FFmpeg extraction"

    echo "Extracting ${archive_name}..."
    7z x "$archive_name"

    extracted_path="$(gyan_ffmpeg_extract_dir)"
    if [[ ! -d "$extracted_path" ]]; then
        echo "Expected extracted directory ${extracted_path} was not found." >&2
        return 1
    fi

    echo "Copying DLLs from ${extracted_path}/bin/ to the current directory..."
    chmod +x "${extracted_path}/bin/"*
    cp "${extracted_path}/bin/"*.dll .

    generate_ffmpeg_source_manifest "$extracted_path" "$archive_name"

    rm -f -- "$archive_name"

    echo "Pinned Gyan FFmpeg build ${GYAN_FFMPEG_VERSION} downloaded and verified successfully."
}

download_sdl_library() {
    local repo_name="$1"
    local file_name="$2"
    local tag="${3:-}"
    local win32_zip_sha256="$4"
    local mingw_tar_sha256="$5"

    local api_url release_data win32_x64_url mingw_url win32_x64_zip mingw_tar
    local win32_x64_dir mingw_dir dll_path

    if [[ -n "$tag" ]]; then
        api_url="https://api.github.com/repos/libsdl-org/${repo_name}/releases/tags/${tag}"
    else
        api_url="https://api.github.com/repos/libsdl-org/${repo_name}/releases/latest"
    fi

    echo "Fetching ${repo_name} release data from GitHub API..."
    if ! release_data="$(github_wget "$api_url")"; then
        echo "Failed to fetch release data from GitHub API for ${repo_name} tag ${tag:-latest}." >&2
        return 1
    fi

    if [[ -z "$release_data" ]]; then
        echo "Empty response when fetching release data from GitHub API for ${repo_name} tag ${tag:-latest}." >&2
        return 1
    fi

    win32_x64_url="$(
        grep -m1 -oE "https://[^\"]+/${file_name}-[^\"]+-win32-x64\\.zip" <<< "$release_data" || true
    )"
    mingw_url="$(
        grep -m1 -oE "https://[^\"]+/${file_name}-devel-[^\"]+-mingw\\.tar\\.gz" <<< "$release_data" || true
    )"

    if [[ -z "$win32_x64_url" ]]; then
        echo "Failed to find the ${file_name} win32-x64 build for tag ${tag:-latest}." >&2
        return 1
    fi
    if [[ -z "$mingw_url" ]]; then
        echo "Failed to find the ${file_name}-devel mingw build for tag ${tag:-latest}." >&2
        return 1
    fi

    win32_x64_zip="$(basename "$win32_x64_url")"
    mingw_tar="$(basename "$mingw_url")"

    echo "Downloading ${win32_x64_zip}..."
    wget "$win32_x64_url" -O "$win32_x64_zip"

    echo "Downloading ${mingw_tar}..."
    wget "$mingw_url" -O "$mingw_tar"

    echo "Verifying SHA-256 for ${win32_x64_zip}..."
    verify_archive_sha256 "$win32_x64_zip" "$win32_zip_sha256"

    echo "Verifying SHA-256 for ${mingw_tar}..."
    verify_archive_sha256 "$mingw_tar" "$mingw_tar_sha256"

    win32_x64_dir="${win32_x64_zip%.zip}"
    mingw_dir="${mingw_tar%.tar.gz}"

    echo "Removing any previous extraction directory ${win32_x64_dir}..."
    remove_tree_if_expected "$win32_x64_dir" "${file_name} win32-x64 extraction"

    echo "Removing any previous extraction directory ${mingw_dir}..."
    remove_tree_if_expected "$mingw_dir" "${file_name} mingw-devel extraction"

    mkdir -p "$win32_x64_dir" "$mingw_dir"

    echo "Extracting ${win32_x64_zip} into ${win32_x64_dir}..."
    unzip -q "$win32_x64_zip" -d "$win32_x64_dir"

    echo "Extracting ${mingw_tar} into ${mingw_dir}..."
    tar -xzf "$mingw_tar" -C "$mingw_dir"

    dll_path="${win32_x64_dir}/${file_name}.dll"
    if [[ ! -f "$dll_path" ]]; then
        echo "${file_name}.dll not found at ${dll_path}" >&2
        return 1
    fi

    echo "Copying ${file_name}.dll to the current directory..."
    chmod +x "$dll_path"
    cp "$dll_path" .

    rm -f -- "$win32_x64_zip" "$mingw_tar"

    echo "${file_name} win32-x64 and mingw-devel builds downloaded and extracted successfully."
}

validate_sdl_tag() {
    local component="$1"
    local supplied_tag="$2"
    local configured_tag="$3"

    if [[ -n "$supplied_tag" && "$supplied_tag" != "$configured_tag" ]]; then
        echo "Custom ${component} tag '${supplied_tag}' has no configured checksum; update windows_deps.mk first." >&2
        return 1
    fi
}

ARG="$(trim_value "${1:-}")"
TAG="$(trim_value "${2:-}")"

main() {
    case "$ARG" in
        ffmpeg)
            download_ffmpeg
            ;;
        sdl2)
            require_sdl2_deps
            validate_sdl_tag "SDL2" "$TAG" "release-${SDL2_VERSION}"
            download_sdl_library "SDL" "SDL2" "release-${SDL2_VERSION}" \
                "$SDL2_WIN32_X64_ZIP_SHA256" "$SDL2_DEVEL_MINGW_TAR_SHA256"
            ;;
        sdl2_ttf)
            require_sdl2_ttf_deps
            validate_sdl_tag "SDL2_ttf" "$TAG" "release-${SDL2_TTF_VERSION}"
            download_sdl_library "SDL_ttf" "SDL2_ttf" "release-${SDL2_TTF_VERSION}" \
                "$SDL2_TTF_WIN32_X64_ZIP_SHA256" "$SDL2_TTF_DEVEL_MINGW_TAR_SHA256"
            ;;
        *)
            echo "Usage: $0 {ffmpeg|sdl2|sdl2_ttf} [release_tag]" >&2
            exit 1
            ;;
    esac
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
