FROM debian:trixie@sha256:9c6e381421d0f48d1b8133e573f33c185ee16cbf505eda9c4a8a4791b25ea8a6 AS debian-base
ENV DEBIAN_FRONTEND=noninteractive
ARG LIBXML2_DEBIAN_VERSION=2.12.7+dfsg+really2.9.14-2.1+deb13u3

# Build a Debian package using the published Ubuntu Noble security backports.
# Debian Trixie has no release update for these advisories yet.
FROM debian-base AS libxml2-backport
ARG PITROVE_BUILD_JOBS
WORKDIR /build-libxml2
RUN set -eu; \
    sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources; \
    apt-get update; \
    apt-get install -y --no-install-recommends build-essential ca-certificates curl dpkg-dev xz-utils; \
    apt-get build-dep -y --arch-only --no-install-recommends "libxml2=$LIBXML2_DEBIAN_VERSION"; \
    apt-get source "libxml2=$LIBXML2_DEBIAN_VERSION"; \
    curl --fail --silent --show-error --location --retry 3 \
        https://archive.ubuntu.com/ubuntu/pool/main/libx/libxml2/libxml2_2.9.14+dfsg-1.3ubuntu3.10.debian.tar.xz \
        --output ubuntu.tar.xz; \
    printf '%s  ubuntu.tar.xz\n' d2d821830eb60edbcfcbe30e2827420e9e439e98c85c812352969cdda764d60b | sha256sum -c -; \
    cd libxml2-*/; \
    for cve in 6653 86140 74860 86138 86142 86143 86144; do \
        patch_name="CVE-2026-$cve.patch"; \
        tar -xOf ../ubuntu.tar.xz "debian/patches/$patch_name" > "debian/patches/$patch_name"; \
        printf '%s\n' "$patch_name" >> debian/patches/series; \
    done; \
    deb_version="$LIBXML2_DEBIAN_VERSION+pitrove1"; \
    printf 'libxml2 (%s) trixie; urgency=high\n\n  * Apply Ubuntu Noble security backports.\n\n -- piTrove <UnDadFeated@piTrove>  Sun, 22 Nov 2026 00:00:00 +0000\n\n' \
        "$deb_version" > /tmp/libxml2-changelog; \
    cat debian/changelog >> /tmp/libxml2-changelog; \
    cp /tmp/libxml2-changelog debian/changelog; \
    DEB_BUILD_OPTIONS="parallel=${PITROVE_BUILD_JOBS:-$(nproc)}" dpkg-buildpackage -B -us -uc; \
    mkdir /out; \
    cp "../libxml2_${deb_version}_$(dpkg --print-architecture).deb" /out/libxml2.deb

# Compile the piTrove executable.
FROM debian-base AS builder

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
FROM debian-base

# Copy Raspberry Pi repository keyring and sources for hardware acceleration
COPY raspberrypi-archive-keyring.pgp /usr/share/keyrings/raspberrypi-archive-keyring.pgp
COPY raspi.sources /etc/apt/sources.list.d/raspi.sources

# Keep the rebuilt .deb out of the final image layers.
RUN --mount=type=bind,from=libxml2-backport,source=/out/libxml2.deb,target=/tmp/libxml2.deb \
    apt-get update && apt-get upgrade -y && apt-get install -y --no-install-recommends \
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
    libv4l-0t64 \
    libv4lconvert0t64 \
    tzdata \
    libgl1 \
    libgl1-mesa-dri \
    && if [ "$(dpkg-query -W -f='${Version}' libxml2)" != "$LIBXML2_DEBIAN_VERSION" ]; then \
        printf '%s\n' 'Review the libxml2 backport for the current Debian package version.' >&2; \
        exit 1; \
    fi \
    && dpkg -i /tmp/libxml2.deb \
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
