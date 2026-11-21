# ============================================================
# Stage 1: Build stage
# ============================================================
FROM debian:trixie@sha256:4ae67669760b807c19f23902a3fd7c121a6a70cf2ae709035674b23e712e4d62 AS builder

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Copy Raspberry Pi repository keyring and sources for hardware acceleration
COPY raspberrypi-archive-keyring.pgp /usr/share/keyrings/raspberrypi-archive-keyring.pgp
COPY raspi.sources /etc/apt/sources.list.d/raspi.sources

# Install C++ compilation tools and library headers
RUN apt-get update && apt-get upgrade -y && apt-get install -y --no-install-recommends \
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
    libical-dev \
    nlohmann-json3-dev \
    libmosquitto-dev \
    liblcms2-dev \
    zlib1g-dev \
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

# Set up build directories
WORKDIR /build-src
COPY src/ /build-src/src/

# Compile piTrove executable target in Release mode
RUN rm -rf /build-src/src/build && mkdir -p /build-src/src/build
WORKDIR /build-src/src/build
ARG PITROVE_BUILD_JOBS
RUN cmake .. -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . --parallel "${PITROVE_BUILD_JOBS:-$(nproc)}"

# ============================================================
# Stage 2: Runtime stage
# ============================================================
FROM debian:trixie@sha256:4ae67669760b807c19f23902a3fd7c121a6a70cf2ae709035674b23e712e4d62

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Copy Raspberry Pi repository keyring and sources for hardware acceleration
COPY raspberrypi-archive-keyring.pgp /usr/share/keyrings/raspberrypi-archive-keyring.pgp
COPY raspi.sources /etc/apt/sources.list.d/raspi.sources

# Install only the runtime libraries, fonts, and utilities
RUN apt-get update && apt-get upgrade -y && apt-get install -y --no-install-recommends \
    libsdl3-0 \
    libsdl3-image0 \
    libsdl3-ttf0 \
    libsqlite3-0 \
    libexif12 \
    libtiff6 \
    libheif1 \
    libical3t64 \
    libheif-plugin-libde265 \
    liblcms2-2 \
    libwebp7 \
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
    libmosquitto1 \
    libsodium23 \
    libcurl4 \
    ca-certificates \
    iputils-ping \
    libstb0t64 \
    libv4l-0t64 \
    libv4lconvert0t64 \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

# SDL loads these libraries and drivers dynamically for KMSDRM and headless rendering.
RUN apt-get update && apt-get install -y --no-install-recommends libgl1 libgl1-mesa-dri \
    && rm -rf /var/lib/apt/lists/*

# Establish runtime directory tree
WORKDIR /app
RUN mkdir -p /app/cache /app/config /app/logs /app/subtitles /app/screenshots /app/media

# Deploy compiled binary
COPY --from=builder /build-src/src/piTrove /app/piTrove

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
