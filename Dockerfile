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
    libavutil-dev \
    libstb-dev \n    libsodium-dev \
    && rm -rf /var/lib/apt/lists/*

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
    libavutil59 \
    imagemagick \
    exiftool \
    dav1d \
    ffmpeg \
    mosquitto-clients \n    libsodium23 \
    ca-certificates \
    curl \
    iputils-ping \
    libstb0t64 \
    && rm -rf /var/lib/apt/lists/*

# Establish runtime directory tree
WORKDIR /app
RUN mkdir -p /app/cache /app/config /app/logs /app/subtitles /app/media

# Deploy compiled binary
COPY --from=builder /build-src/src/piTrove /app/piTrove

# Deploy static default fonts, assets, and configs
COPY src/fonts/ /app/src/fonts/
COPY src/splash.png /app/src/splash.png
COPY src/config.toml /app/src/config/config.toml

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
    CMD curl -sf http://localhost:9000/api/status || exit 1
