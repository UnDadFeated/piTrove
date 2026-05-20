# piTrove

A professional-grade digital picture frame for the Raspberry Pi. Designed for extreme stability on network-attached storage (NAS), cinematic visual presentation, and seamless hardware acceleration on Pi 4 and Pi 5.

[![Platform](https://img.shields.io/badge/platform-Pi%204%20%7C%20Pi%205-blue?style=flat-square)](https://www.raspberrypi.com/)
[![OS](https://img.shields.io/badge/OS-Trixie%20Lite%20%28Debian%2013%29-lightgreen?style=flat-square)](https://www.debian.org/)
[![Architecture](https://img.shields.io/badge/arch-aarch64-orange?style=flat-square)](https://en.wikipedia.org/wiki/AArch64)
[![Graphics](https://img.shields.io/badge/graphics-DRM%2FKMS%20%7C%20GLES2-red?style=flat-square)](https://www.mesa3d.org/)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)

## 🚀 Quick Start

### 1. OS Installation
1. Flash **Raspberry Pi OS Lite (64-bit)** using the [Raspberry Pi Imager](https://www.raspberrypi.com/software/).
2. Ensure you are using a 64-bit image; the application requires `aarch64`.
3. Boot the Pi, connect to your network, and SSH in: `ssh pi@<pi-ip>`.

### 2. One-Command Installation
Run the bootstrap script to handle dependencies, build the binary, and configure the systemd service:
```bash
wget -qO- https://raw.githubusercontent.com/UnDadFeated/piTrove/main/install.sh | sudo bash
```

---

## ✨ Key Features

### 🖼️ High-Performance Media Engine
- **Broad Format Support**: Native loading for JPEG, PNG, TIFF, WebP, HEIC/HEIF, BMP, and TGA.
- **Cinematic Visuals**: 
  - **Ken Burns Effect**: Smooth, configurable zoom and pan animations.
  - **Professional Transitions**: High-quality crossfades, wipes, and pixelate effects.
  - **Dynamic Ambient Lighting**: Photo-aware edge glow and bias lighting that blends the frame into the room.
  - **CRT Aesthetic**: Optional vignette and scanline overlays for a retro-digital look.
- **Video Integration**: Seamless interleaving of H.264/H.265 videos using `libmpv` with hardware acceleration (`drmprime-copy`).

### 📂 Enterprise-Grade Scanning & Cache
- **NAS Optimized**: Specialized `getdents64` implementation with timeout wrappers to prevent the "CIFS hang" common in standard filesystem libraries.
- **SQLite3 Persistence**: Uses a WAL-mode database to track file metadata, preventing redundant scans and tracking corrupted files to skip them permanently.
- **Temporal Filtering**: A "Seasonal Window" filter shows photos from the current time of year across any year (e.g., show only "December" photos every December).
- **Intelligent Cooldown**: Ensures the same photo isn't shown too frequently (default 330-day cooldown).

### 🛠️ System & Control
- **Headless Design**: Operates via DRM/KMS (native framebuffer). No X11 or Wayland required.
- **Low Power**: Scheduled display sleep/wake times and automatic backlight dimming.
- **Remote Control**: Built-in HTTP API for remote navigation (`/api/next`, `/api/prev`, `/api/pause`).
- **TUI Configurator**: A robust, adaptive terminal-based wizard for configuring all aspects of the engine over SSH.

---

## ⚙️ Technical Architecture

```mermaid
graph TD
    A[Media Root] --> B[Phase 1: Recursive Scan]
    B --> C[Phase 2: SQLite Metadata Cache]
    C --> D[Phase 3: Async Preload Pipeline]
    D --> E[GLES2 Render Loop]
    E --> F[DRM/KMS Framebuffer]
    
    subgraph "Preload Pipeline"
    D1[Current Image]
    D2[Next Image]
    D3[Buffer Image]
    end
```

- **Language**: C++17 / OpenGL ES 2.0
- **Core Libraries**: Raylib, libmpv, SQLite3, libexif, libheif.
- **Hardware Accel**: Pi 5 V4L2 / VC4 DRM path.

## 📁 Project Structure
- `src/piTrove.cpp`: The core application logic.
- `src/config/config.toml`: User configuration.
- `install.sh`: Bootstrap installer.
- `CHANGELOG.md`: Detailed version history.

---
MIT License
