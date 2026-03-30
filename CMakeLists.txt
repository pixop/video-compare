cmake_minimum_required(VERSION 3.20)
project(video-compare VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ---
option(ENABLE_GUI "Build with Dear ImGui GUI support" ON)

# --- Dependencies ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavfilter libavutil libswscale libswresample)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

# --- Dear ImGui via FetchContent (docking branch) ---
if(ENABLE_GUI)
    include(FetchContent)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG docking
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui_lib PUBLIC PkgConfig::SDL2)
endif()

# --- Source files ---
file(GLOB SRC_FILES src/*.cpp)

set(ALL_SOURCES ${SRC_FILES})

if(ENABLE_GUI)
    file(GLOB GUI_FILES gui/*.cpp)
    list(APPEND ALL_SOURCES ${GUI_FILES})
endif()

# --- Executable ---
add_executable(video-compare ${ALL_SOURCES})

target_include_directories(video-compare PRIVATE
    src/
    vendor/
)

target_link_libraries(video-compare PRIVATE
    PkgConfig::FFMPEG
    PkgConfig::SDL2
    PkgConfig::SDL2_TTF
)

if(ENABLE_GUI)
    target_include_directories(video-compare PRIVATE
        gui/
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(video-compare PRIVATE imgui_lib)
    target_compile_definitions(video-compare PRIVATE ENABLE_GUI=1)
endif()

# --- Compiler flags ---
target_compile_definitions(video-compare PRIVATE __STDC_CONSTANT_MACROS)
target_compile_options(video-compare PRIVATE
    -Wall -Wextra
    -Wno-deprecated -Wno-deprecated-declarations
    -Wno-unused -Wno-missing-field-initializers
)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    target_compile_options(video-compare PRIVATE -O2)
endif()

# --- Install ---
install(TARGETS video-compare RUNTIME DESTINATION bin)
