# ============================================================
# Stage 1: Build stage
# ============================================================
FROM debian:trixie AS builder

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Copy Raspberry Pi repository keyring and sources for hardware acceleration
COPY raspberrypi-archive-keyring.pgp /usr/share/keyrings/raspberrypi-archive-keyring.pgp
COPY raspi.sources /etc/apt/sources.list.d/raspi.sources

# Install C++ compilation tools and library headers
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    libsdl3-dev \
    libsdl3-image-dev \
    libsdl3-ttf-dev \
    libsqlite3-dev \
    libexif-dev \
    libjpeg-dev \
    libpng-dev \
    libtiff-dev \
    libheif-dev \
    libwebp-dev \
    libjpeg62-turbo-dev \
    libopenjp2-7-dev \
    libraw-dev \
    libasound2-dev \
    libfreetype6-dev \
    libfontconfig1-dev \
    libdrm-dev \
    libgbm-dev \
    libegl1-mesa-dev \
    libgles2-mesa-dev \
    libavcodec-dev \
    libavformat-dev \
    libswscale-dev \
    libswresample-dev \
    libavutil-dev \
    libstb-dev \
    libsodium-dev \
    libcurl4-openssl-dev \
    libv4l-dev \
    && rm -rf /var/lib/apt/lists/*

# Build FFmpeg 7.1.5 with --enable-v4l2-m2m (kernel M2M HW decode).
# The RPI/Debian libav* 7.1.5 packages ship WITHOUT the v4l2_m2m hwcontext, so
# hevc_v4l2m2m is missing and HEVC decoding silently falls back to software
# (~10fps on 4K60). Same upstream source as the system libs (Debian pool
# 7.1.5-0+deb13u1) => ABI-identical sonames (libavutil.so.59, libavcodec.so.61).
RUN apt-get update && apt-get install -y --no-install-recommends wget xz-utils \
    && cd /tmp \
    && wget -q http://deb.debian.org/debian/pool/main/f/ffmpeg/ffmpeg_7.1.5.orig.tar.xz \
    && wget -q http://deb.debian.org/debian/pool/main/f/ffmpeg/ffmpeg_7.1.5-0+deb13u1.debian.tar.xz \
    && tar -xf ffmpeg_7.1.5.orig.tar.xz \
    && tar -xf ffmpeg_7.1.5-0+deb13u1.debian.tar.xz \
    && cd ffmpeg-7.1.5 \
    && ./configure --prefix=/opt/ffmpeg --enable-shared --enable-v4l2-m2m --disable-doc \
    && make -j$(nproc) \
    && make install \
    && cd / && rm -rf /tmp/ffmpeg-7.1.5 /tmp/ffmpeg_7.1.5.orig.tar.xz /tmp/ffmpeg_7.1.5-0+deb13u1.debian.tar.xz
# Compile+link the app against the custom FFmpeg via pkg-config
ENV PKG_CONFIG_PATH=/opt/ffmpeg/lib/pkgconfig

# Set up build directories
WORKDIR /build-src
COPY src/ /build-src/src/

# Compile piTrove executable target in Release mode
RUN rm -rf /build-src/src/build && mkdir -p /build-src/src/build
WORKDIR /build-src/src/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . -j$(nproc)

# ============================================================
# Stage 2: Runtime stage
# ============================================================
FROM debian:trixie

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Copy Raspberry Pi repository keyring and sources for hardware acceleration
COPY raspberrypi-archive-keyring.pgp /usr/share/keyrings/raspberrypi-archive-keyring.pgp
COPY raspi.sources /etc/apt/sources.list.d/raspi.sources

# Install only the runtime libraries, fonts, and utilities
RUN apt-get update && apt-get install -y --no-install-recommends \
    libsdl3-0 \
    libsdl3-image0 \
    libsdl3-ttf0 \
    libsqlite3-0 \
    libexif12 \
    libasound2t64 \
    libfreetype6 \
    libfontconfig1 \
    libdrm2 \
    libgbm1 \
    libegl1 \
    libgles2 \
    libavcodec61 \
    libavformat61 \
    libswscale8 \
    libswresample5 \
    libavutil59 \
    imagemagick \
    exiftool \
    dav1d \
    ffmpeg \
    mosquitto-clients \
    libsodium23 \
    libcurl4 \
    ca-certificates \
    curl \
    iputils-ping \
    libstb0t64 \
    libv4l-0t64 \
    libv4lconvert0t64 \
    v4l-utils \
    network-manager \
    sudo \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

# Establish runtime directory tree
WORKDIR /app
RUN mkdir -p /app/cache /app/config /app/logs /app/subtitles /app/screenshots /app/media

# Deploy compiled binary
COPY --from=builder /build-src/src/piTrove /app/piTrove

# Deploy the custom FFmpeg shared libs (v4l2_m2m HW decode enabled).
# /usr/local/lib is searched before /lib/aarch64-linux-gnu, so /app/piTrove
# resolves the v4l2_m2m-enabled sonames; system libav* remain as fallback.
COPY --from=builder /opt/ffmpeg/lib/ /usr/local/lib/
RUN ldconfig

# Deploy static default fonts, assets, and configs
COPY src/fonts/ /app/src/fonts/
COPY src/splash.png /app/src/splash.png
COPY src/config.toml /app/src/config/config.toml
COPY scripts/ /app/scripts/
RUN chmod +x /app/scripts/*.sh 2>/dev/null || true

# Expose HTTP control interface dashboard
EXPOSE 9000

# Configure Direct-to-Framebuffer KMSDRM environment variables
ENV HOME=/app
ENV XDG_CACHE_HOME=/app/cache
ENV MESA_SHADER_CACHE_DIR=/app/cache/mesa_shader_cache
ENV SDL_VIDEO_DRIVER=kmsdrm

# Execute digital slideshow
ENTRYPOINT ["/app/piTrove", "--config", "/app/config/config.toml"]

# Container Health Check
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD /app/scripts/healthcheck.sh
