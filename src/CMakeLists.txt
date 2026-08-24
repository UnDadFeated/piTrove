cmake_minimum_required(VERSION 3.16)
project(piTrove VERSION 18.0.2 LANGUAGES CXX)

option(ENABLE_ASAN "Enable AddressSanitizer for memory safety testing" OFF)
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer (ASan) enabled")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ARM64 optimization
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=armv8-a+crc+simd -mtune=cortex-a72 -fomit-frame-pointer -pipe")
    # LTO removed in v15.6.9 to prevent FFmpeg SIMD miscompilation
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
pkg_check_modules(CURL REQUIRED libcurl)

# EXIF
find_library(EXIF_LIBRARY NAMES exif REQUIRED)

# STB image
pkg_check_modules(STB stb)

# FFmpeg (for in-process video decoding)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(AVUTIL REQUIRED libavutil)
# SWResample
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

# ============================================================
# Source files
# ============================================================

set(PISTROVE_SOURCES
    main.cpp
    auth.cpp
    health.cpp
    safe_mode.cpp
    thermal.cpp
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
    blur.cpp
    util.cpp
    http_server.cpp
    mqtt.cpp
    google_photos.cpp
    error_db.cpp
    organizer.cpp
    preprocess.cpp
    news_ticker.cpp
    calendar.cpp
)

# Video decoder (FFmpeg)
list(APPEND PISTROVE_SOURCES video_decoder.cpp)

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
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    /usr/include/drm
)

target_link_libraries(piTrove PRIVATE
    sodium
    ${SDL3_LIBRARIES}
    ${SDL3_IMAGE_LIBRARIES}
    ${SDL3_TTF_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${DRM_LIBRARY}
    ${GBM_LIBRARY}
    ${EGL_LIBRARY}
    ${GLES2_LIBRARY}
    ${SQLITE3_LIBRARIES}
    ${CURL_LIBRARIES}
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
target_compile_options(piTrove PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Output binary in parent of src/ (repo root)
set_target_properties(piTrove PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/..")

# ============================================================
# Install
# ============================================================

install(TARGETS piTrove DESTINATION ..)
