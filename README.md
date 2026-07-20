# piTrove — C++ Digital Picture & Video Frame for Raspberry Pi

<p align="center">
<a href="https://www.raspberrypi.com/"><img src="https://img.shields.io/badge/platform-Pi%204%20%7C%20Pi%205-blue?style=flat-square" alt="Platform" /></a>
<a href="https://www.debian.org/"><img src="https://img.shields.io/badge/OS-Trixie%20Lite%20%28Debian%2013%29-lightgreen?style=flat-square" alt="OS" /></a>
<a href="https://en.wikipedia.org/wiki/AArch64"><img src="https://img.shields.io/badge/arch-aarch64-orange?style=flat-square" alt="Arch" /></a>
<a href="https://www.libsdl.org/"><img src="https://img.shields.io/badge/graphics-SDL3-red?style=flat-square" alt="Graphics" /></a>
<img src="https://img.shields.io/github/stars/UnDadFeated/piTrove?style=flat-square&logo=github" alt="Stars" />
</p>

Professional-grade, **containerized** digital picture & video frame application for Raspberry Pi 4 & 5. C++17, DRM/KMS GPU rendering, hardware-accelerated video decoding, NAS storage support, and zero-touch auto-updates.

🌐 **[Live Documentation & Landing Page](https://undadfeated.github.io/piTrove/)**

---

## 🚀 Quick Start

### 1. OS Installation

1. Flash **Raspberry Pi OS Lite (64-bit)** using the [Raspberry Pi Imager](https://www.raspberrypi.com/software/).
2. Boot, connect to your network, and SSH in: `ssh pi@<pi-ip>`

### 2. One-Command Installation

```bash
wget -qO- https://raw.githubusercontent.com/UnDadFeated/piTrove/main/install.sh | sudo bash
```

The installer handles everything: Docker Engine, NAS mount configuration, DRM/KMS setup, container build, systemd service, and auto-update cron job.

### 3. Management

```bash
pitrove config    # Interactive TUI settings wizard
pitrove restart   # Restart the slideshow
pitrove logs      # Real-time render logs
pitrove status    # Container health
```

```bash
# Update application
sudo ./install.sh --update

# Reorganize media archive
sudo ./install.sh --organize /path/to/archive
```

---

## ✨ Key Features

### 🖼 Media Engine

- **Broad Format Support**: JPEG, PNG, TIFF, WebP, HEIC/HEIF, BMP, TGA
- **Video Playback**: H.264/H.265/AV1 in-process FFmpeg decode with hardware acceleration
- **Pi 4/5 Auto-Detection**: DRM hwaccel (Pi 5) or V4L2 M2M (Pi 4) with software fallback
- **Twin-Portrait Collage**: Side-by-side portrait layout for adjacent photos
- **Ken Burns Effect**: Smooth zoom and pan animations
- **Professional Transitions**: Crossfade, wipe, pixelate, dissolve
- **Dynamic Bias Lighting**: Photo-aware edge glow with 5 animation styles
- **Adaptive Overlays**: Filename, date, countdown timer, clock, diagnostics HUD
- **CRT Aesthetic**: Optional vignette and scanline overlays

### 📂 Smart Scanning & Cache

- **NAS-Optimized**: CIFS hang prevention with timeout wrappers
- **SQLite3 WAL Persistence**: Zero redundant scans across restarts
- **Seasonal Window**: Month-based temporal filtering
- **On-This-Day**: Anniversary photo matching with configurable tolerance
- **Intelligent Cooldown**: 330-day default rotation cycle
- **Content Filtering**: Auto-filter screenshots, documents, receipts
- **Preload Pipeline**: Multi-threaded async image preloading

### 🛰 Integrations

- **MQTT & Home Assistant**: Auto-discovery, remote controls, motion sensor integration
- **Google Photos**: Cloud sync with OAuth2 refresh tokens
- **HTTP Dashboard**: Glassmorphic web control panel with live telemetry
- **Touchscreen Control**: Native evdev input with floating navigation
- **Sleep/Wake Scheduling**: Automatic display power management

### 🛡 Reliability

- **Docker Containerized**: Full dependency isolation, build-once deployment
- **Software Watchdog**: In-app heartbeat with Docker + systemd restart chain
- **Network Recovery**: Gateway monitoring with automatic Wi-Fi reset
- **Video Stall Detection**: 30s timeout with forced decoder recovery
- **Async-Signal-Safe**: Pre-allocated crash handlers, no heap in signal paths
- **OLED Burn-In Prevention**: Pixel shifting and content rotation
- **Auto-Update System**: Daily cron-based updates with config merge
- **PIN-Protected Dashboard**: 4-digit access lock

---

## ⚙️ Technical Architecture

```
Media Root → Recursive Scan → SQLite Cache → Async Preload → SDL3 Render Loop → DRM/KMS
```

- **Language**: C++17
- **Core**: SDL3, SDL3_image, SDL3_ttf, FFmpeg (libavcodec/libavformat/libswscale/libswresample), SQLite3, libexif, libheif
- **HW Accel**: Pi 5 DRM (vc4-kms-v3d) / Pi 4 V4L2 M2M / SW fallback
- **Target**: Debian Trixie Lite 64-bit on Raspberry Pi 4 & 5

---

## 📂 Project Structure

```
src/
├── main.cpp              — Entry point, render loop, event handling
├── video_decoder.cpp/h   — FFmpeg decode + hwaccel + audio
├── transition.cpp/h      — GPU transitions (crossfade, wipe, pixelate, dissolve, ken burns)
├── overlay.cpp/h         — OSD widgets (timer, date, filename, clock, diagnostics)
├── font_render.cpp/h     — Cached + uncached text rendering
├── renderer.cpp/h        — SDL3 primitives, bias lighting, CRT vignette
├── image_loader.cpp/h    — Multi-format image loading
├── scanner.cpp/h         — Recursive media scanning (getdents64)
├── cache.cpp/h           — SQLite3 WAL-mode metadata persistence
├── preload.cpp/h         — Async image preloading queue
├── config.cpp/h          — TOML config parser & validation
├── tui.cpp/h             — Terminal configuration wizard
├── mqtt.cpp/h            — MQTT subscriber & Home Assistant discovery
├── google_photos.cpp/h   — Google Photos cloud sync
├── http_server.cpp/h     — Web dashboard & remote control API
├── organizer.cpp/h       — Media archive organizer
├── blur.cpp/h            — Box blur filter & matte color computation
├── error_db.cpp/h        — Diagnostic error catalog
├── health.cpp/h          — System health monitoring
├── thermal.cpp/h         — SoC thermal monitoring
├── safe_mode.cpp/h       — Crash recovery & safe mode
├── auth.cpp/h            — Dashboard authentication
├── preprocess.cpp/h      — Background EXIF preprocessing
├── util.cpp/h            — Utilities, string parsing, safety
├── media_item.h          — Media data structures
├── interfaces.h          — Interface definitions
├── expected.h            — Expected type implementation
├── fonts/                — DejaVuSansMono-Bold.ttf
├── config.toml           — Default configuration
└── splash.png            — Boot splash screen
```

- `install.sh`: Bootstrap installer (Docker, NAS, systemd, auto-update)
- `CHANGELOG.md`: Detailed version history


## License

GNU GPLv3 (or later) License
