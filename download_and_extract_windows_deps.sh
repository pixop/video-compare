#!/bin/bash

# Function to download and extract the latest FFmpeg full shared build
download_ffmpeg() {
    BASE_URL="https://www.gyan.dev/ffmpeg/builds/"
    echo "Fetching the FFmpeg builds page..."
    PAGE_CONTENT=$(wget -qO- "$BASE_URL")

    LATEST_LINK=$(echo "$PAGE_CONTENT" | grep -oP '(?<=href=")[^"]*ffmpeg-release-full-shared.*\.7z(?=")' | head -n 1)
    if [ -z "$LATEST_LINK" ]; then
        echo "Failed to find the latest FFmpeg full shared build."
        return 1
    fi

    DOWNLOAD_URL="$LATEST_LINK"
    echo "Downloading $DOWNLOAD_URL..."
    wget "$DOWNLOAD_URL" -O "ffmpeg-release-full-shared.7z"

    echo "Extracting ffmpeg-release-full-shared.7z..."
    7z x "ffmpeg-release-full-shared.7z"

    EXTRACTED_DIR=$(ls -dt ffmpeg*/ | head -n 1)
    if [ -z "$EXTRACTED_DIR" ]; then
        echo "Failed to extract the archive."
        return 1
    fi

    echo "Copying DLLs from $EXTRACTED_DIR/bin/ to the current directory..."
    chmod +x "$EXTRACTED_DIR/bin/"*
    cp "$EXTRACTED_DIR/bin/"*.dll .
    rm "ffmpeg-release-full-shared.7z"
    echo "FFmpeg full shared build downloaded and extracted successfully."
}

# Function to download and extract the SDL library
# Parameters:
#   1. GitHub repo name (e.g., SDL or SDL_ttf)
#   2. Base filename for assets (e.g., SDL2 or SDL2_ttf)
#   3. (Optional) GitHub release tag
download_sdl_library() {
    REPO_NAME=$1
    FILE_NAME=$2
    TAG=$3

    if [ -n "$TAG" ]; then
        API_URL="https://api.github.com/repos/libsdl-org/$REPO_NAME/releases/tags/$TAG"
    else
        API_URL="https://api.github.com/repos/libsdl-org/$REPO_NAME/releases/latest"
    fi

    echo "Fetching $REPO_NAME release data from GitHub API..."
    RELEASE_DATA=$(wget -qO- "$API_URL")

    if [ -z "$RELEASE_DATA" ]; then
        echo "Failed to fetch release data from GitHub API. Check if the tag is correct."
        return 1
    fi

    # Extract the win32-x64 and mingw asset URLs
    WIN32_X64_URL=$(echo "$RELEASE_DATA" | grep -oP "(?<=\"browser_download_url\": \")[^\"]*$FILE_NAME-[^\"]*-win32-x64\.zip")
    MINGW_URL=$(echo "$RELEASE_DATA" | grep -oP "(?<=\"browser_download_url\": \")[^\"]*$FILE_NAME-devel-[^\"]*-mingw\.tar\.gz")

    if [ -z "$WIN32_X64_URL" ]; then
        echo "Failed to find the $FILE_NAME win32-x64 build."
        return 1
    fi
    if [ -z "$MINGW_URL" ]; then
        echo "Failed to find the $FILE_NAME-devel mingw build."
        return 1
    fi

    # Download the win32-x64 and mingw assets
    WIN32_X64_ZIP=$(basename "$WIN32_X64_URL")
    MINGW_TAR=$(basename "$MINGW_URL")
    
    echo "Downloading $WIN32_X64_ZIP..."
    wget "$WIN32_X64_URL" -O "$WIN32_X64_ZIP"
    
    echo "Downloading $MINGW_TAR..."
    wget "$MINGW_URL" -O "$MINGW_TAR"

    # Create directories based on zip/tar file names (without extension)
    WIN32_X64_DIR="${WIN32_X64_ZIP%.zip}"
    MINGW_DIR="${MINGW_TAR%.tar.gz}"
    mkdir -p "$WIN32_X64_DIR" "$MINGW_DIR"

    # Extract the archives
    echo "Extracting $WIN32_X64_ZIP into $WIN32_X64_DIR..."
    unzip -q "$WIN32_X64_ZIP" -d "$WIN32_X64_DIR"

    echo "Extracting $MINGW_TAR into $MINGW_DIR..."
    tar -xzf "$MINGW_TAR" -C "$MINGW_DIR"

    # Copy the DLL to the current directory (if it exists)
    DLL_PATH="$WIN32_X64_DIR/$FILE_NAME.dll"
    if [ -f "$DLL_PATH" ]; then
        echo "Copying $FILE_NAME.dll to the current directory..."
        chmod +x "$DLL_PATH"
        cp "$DLL_PATH" .
    else
        echo "$FILE_NAME.dll not found in $DLL_PATH"
    fi

    # Cleanup downloaded files
    rm "$WIN32_X64_ZIP" "$MINGW_TAR"
    echo "$FILE_NAME win32-x64 and mingw-devel builds downloaded and extracted successfully."
}

case "$1" in
    ffmpeg)
        download_ffmpeg
        ;;
    sdl2)
        download_sdl_library "SDL" "SDL2" "$2"
        ;;
    sdl2_ttf)
        download_sdl_library "SDL_ttf" "SDL2_ttf" "$2"
        ;;
    *)
        echo "Usage: $0 {ffmpeg|sdl2|sdl2_ttf} [release_tag]"
        exit 1
        ;;
esac
