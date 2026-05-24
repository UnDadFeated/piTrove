cmake_minimum_required(VERSION 3.16)
project(piTrove VERSION 11.1.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ARM64 optimization
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -mtune=native")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fomit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe")
endif()

# ============================================================
# Dependencies
# ============================================================

find_package(PkgConfig REQUIRED)

# SDL3 + image + ttf
pkg_check_modules(SDL3 REQUIRED sdl3)
pkg_check_modules(SDL3_IMAGE REQUIRED sdl3-image)
pkg_check_modules(SDL3_TTF REQUIRED sdl3-ttf)

# DRM/GLES/EGL/GBM (system libraries on Pi)
find_library(DRM_LIBRARY NAMES drm REQUIRED)
find_library(GBM_LIBRARY NAMES gbm REQUIRED)
find_library(EGL_LIBRARY NAMES EGL REQUIRED)
find_library(GLES2_LIBRARY NAMES GLESv2 REQUIRED)

# SQLite3
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# EXIF
find_library(EXIF_LIBRARY NAMES exif REQUIRED)

# STB image
pkg_check_modules(STB stb)

# MPV (optional — for video)
option(ENABLE_MPV "Enable mpv video playback" ON)
if(ENABLE_MPV)
    find_library(LIBMPV mpv)
    if(LIBMPV)
        add_definitions(-DHAVE_MPV)
    endif()
endif()

# ============================================================
# Source files
# ============================================================

set(PISTROVE_SOURCES
    main.cpp
    config.cpp
    tui.cpp
    scanner.cpp
    cache.cpp
    renderer.cpp
    image_loader.cpp
    font_render.cpp
    transition.cpp
    overlay.cpp
    preload.cpp
    util.cpp
    http_server.cpp
    mqtt.cpp
)

# Optional: video player
if(ENABLE_MPV AND LIBMPV)
    list(APPEND PISTROVE_SOURCES mpv_player.cpp)
endif()

# ============================================================
# Executable
# ============================================================

add_executable(piTrove ${PISTROVE_SOURCES})

target_include_directories(piTrove PRIVATE
    ${SDL3_INCLUDE_DIRS}
    ${SDL3_IMAGE_INCLUDE_DIRS}
    ${SDL3_TTF_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${STB_INCLUDE_DIRS}
    /usr/include/drm
    .
)

target_link_libraries(piTrove PRIVATE
    ${SDL3_LIBRARIES}
    ${SDL3_IMAGE_LIBRARIES}
    ${SDL3_TTF_LIBRARIES}
    ${LIBMPV}
    ${DRM_LIBRARY}
    ${GBM_LIBRARY}
    ${EGL_LIBRARY}
    ${GLES2_LIBRARY}
    ${SQLITE3_LIBRARIES}
    ${EXIF_LIBRARY}
    ${STB_LIBRARIES}
    png16
    jpeg
    webp
    tiff
    heif
    dl
    pthread
    atomic
)

# Compiler flags
target_compile_options(piTrove PRIVATE -O2 -Wall -Wextra)

# Output binary in parent of src/ (repo root)
set_target_properties(piTrove PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/..")

# ============================================================
# Install
# ============================================================

install(TARGETS piTrove DESTINATION ..)
