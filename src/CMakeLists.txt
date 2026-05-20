# CMakeLists.txt — PiTrove Digital Picture Frame
# v5.5.0  |  ARM64  |  Raylib 6.x (DRM/KMS)

cmake_minimum_required(VERSION 3.20)
project(piTrove VERSION 7.10.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output binary in parent of src/ (repo root), not in build/


# ARM64 optimization
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -mtune=native")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fomit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe")
endif()

include_directories(/usr/local/include /usr/include/drm)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(EXIF REQUIRED libexif)
pkg_check_modules(JPEG REQUIRED libjpeg)
pkg_check_modules(PNG REQUIRED libpng)
pkg_check_modules(TIFF REQUIRED libtiff-4)
pkg_check_modules(HEIF REQUIRED libheif)
pkg_check_modules(WEBP REQUIRED libwebp)

set(LIBS
    /usr/local/lib/libraylib.a
    sqlite3
    exif
    jpeg
    png
    tiff
    heif
    webp
    stdc++
    pthread
    dl
    m
    asound
    drm
    gbm
    EGL
    GLESv2
    atomic
    mpv
    avcodec
    avformat
    avutil
    swscale
    ass
)

add_executable(piTrove piTrove.cpp)
target_link_libraries(piTrove PRIVATE ${LIBS})

install(TARGETS piTrove DESTINATION bin)
install(FILES config.toml DESTINATION config)
install(FILES splash.png DESTINATION .) # splash.png is optional user-provided
