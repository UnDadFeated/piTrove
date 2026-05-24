# piTrove

A professional-grade, **containerized** digital picture frame for the Raspberry Pi. Orchestrated via **Docker & Compose** for absolute dependency isolation and read-only media storage safety. Designed for extreme stability on network-attached storage (NAS), cinematic visual presentation, smart home integrations (MQTT + Home Assistant Auto-Discovery), and seamless hardware acceleration on Pi 4 and Pi 5.

[![Platform](https://img.shields.io/badge/platform-Pi%204%20%7C%20Pi%205-blue?style=flat-square)](https://www.raspberrypi.com/)
[![Docker](https://img.shields.io/badge/container-Docker%20%7C%20Compose-blueviolet?style=flat-square&logo=docker)](https://www.docker.com/)
[![OS](https://img.shields.io/badge/OS-Trixie%20Lite%20%28Debian%2013%29-lightgreen?style=flat-square)](https://www.debian.org/)
[![Architecture](https://img.shields.io/badge/arch-aarch64-orange?style=flat-square)](https://en.wikipedia.org/wiki/AArch64)
[![Graphics](https://img.shields.io/badge/graphics-SDL3-red?style=flat-square)](https://www.libsdl.org/)
[![Version](https://img.shields.io/badge/version-11.1.9-blue?style=flat-square)]()

## 🚀 Quick Start

### 1. OS Installation
1. Flash **Raspberry Pi OS Lite (64-bit)** using the [Raspberry Pi Imager](https://www.raspberrypi.com/software/).
2. Ensure you are using a 64-bit image; the application requires `aarch64`.
3. Boot the Pi, connect to your network, and SSH in: `ssh pi@<pi-ip>`.

### 2. One-Command Containerized Installation
Run the bootstrap script to automatically install Docker Engine, mount your storage, build the multi-stage slideshow container, and register the systemd daemon:
```bash
wget -qO- https://raw.githubusercontent.com/UnDadFeated/piTrove/main/install.sh | sudo bash
```

### 3. Management & Interaction
Once the installation completes, the picture frame runs automatically in the background on startup. You can manage the application using standard Docker Compose or systemd controls:

- **Configure the Frame (Interactive TUI)**:
  Launch the premium 8-tab terminal configuration settings wizard inside the container using:
  ```bash
  docker compose exec -it pitrove /app/piTrove --config /app/config/config.toml
  ```
- **Restart Slideshow**:
  ```bash
  sudo systemctl restart piTrove.service
  ```
- **View Slide rendering Logs**:
  ```bash
  docker logs -f piTrove
  ```

---

## ✨ Key Features

### 🖼️ High-Performance Media Engine
- **Broad Format Support**: Native loading for JPEG, PNG, TIFF, WebP, HEIC/HEIF, BMP, and TGA.
- **Cinematic Visuals**: 
  - **Twin-Portrait Collage**: Automatically groups adjacent portrait-format images to display them side-by-side in a split-screen collage with individual frame borders and animations.
  - **Ken Burns Effect**: Smooth, configurable zoom and pan animations.
  - **Professional Transitions**: High-quality crossfades, wipes, pixelate, and dissolve effects.
  - **Dynamic Ambient Lighting**: Photo-aware edge glow and bias lighting that blends the frame into the room.
  - **CRT Aesthetic**: Optional vignette and scanline overlays for a retro-digital look.
- **Video Integration**: Seamless interleaving of H.264/H.265 videos using an accelerated `mpv` subprocess rendering directly via native DRM/KMS.
- **Dynamic CPU Core Scaling**: Dynamically detects available hardware cores and allocates `max_cores - 1` decoding threads to play videos, maximizing hardware efficiency while keeping a core free for background system integrity.

### 📂 Enterprise-Grade Scanning & Cache
- **NAS Optimized**: Specialized `getdents64` implementation with timeout wrappers to prevent the "CIFS hang" common in standard filesystem libraries.
- **SQLite3 Persistence**: Uses a WAL-mode database to track file metadata, preventing redundant scans and tracking corrupted files to skip them permanently.
- **Smart Content Filtering**: Built-in zero-overhead deterministic classifier to filter out non-photographic clutter and prioritize family/wildlife photos.
  - **Keep People**: Automatically detects and keeps photos of family, friends, trips, and portraits based on comprehensive path keyword analysis and hash distribution.
  - **Keep Animals**: Intelligently retains photos containing family pets, wildlife, and animal encounters.
  - **Auto-Filter Clutter**: Automatically identifies and filters out screenshots, scanned documents, receipts, spreadsheets, text graphics, and system icons by default.
  - **TUI Integration**: Toggle filters on the fly via the TUI config wizard (`Keep People` and `Keep Animals` options) or within `config.toml`.
- **Temporal Filtering**: A "Seasonal Window" filter shows photos from the current time of year across any year (e.g., show only "December" photos every December).
- **Intelligent Cooldown**: Ensures the same photo isn't shown too frequently (default 330-day cooldown).
- **Robust Skip Pipeline**: Gracefully intercepts deleted, missing, or corrupt assets, dynamically marking them bad in the cache database and removing them from the queue to prevent application interruptions.

### 🛰️ MQTT & Home Assistant Smart Home Integration
- **Lightweight Subscriber Pipe**: Spawns a background subprocess listener executing `mosquitto_sub -F "%t:%p"` to receive remote controls instantly and safely with zero rendering loop delay.
- **Home Assistant Auto-Discovery**: Automatically publishes standard JSON config payloads to `homeassistant/` on startup. Instantly registers:
  - **Screen Switch** (Toggles physical backlight and solid black blanking overlay)
  - **Skip Next & Previous Buttons** (Remote slideshow navigation)
  - **Play/Pause Toggle Button** (Remote execution control)
  - **Motion Binary Sensor** (Auto-syncs with physical room motion)
- **Automatic Cooldown Blanking**: Monitors the room's motion sensor topic. If no motion is detected within a customizable cooldown window, it blanks the screen physically using `vcgencmd display_power 0` and clears the framebuffer to black. Wakes up instantly on new motion or key down/mouse events.

### 🛠️ System & Control
- **Headless Design**: Operates via DRM/KMS (native framebuffer). No X11 or Wayland required.
- **Dynamic Display & GPU Probing**: Programmatically queries active connected DRM/KMS connector outputs and indices (sysfs `card*-*/status`), auto-configuring stable KMSDRM environments before SDL3 starts up.
- **Low Power**: Scheduled display sleep/wake times and automatic backlight dimming.
- **Glassmorphic HTTP HUD Remote**: Built-in HTTP web dashboard featuring interactive player controls, dynamic slideshow diagnostics telemetry (temp, cache DB size, queue size), active MQTT broker connection cards, screen switches, and simulated motion triggers.
- **TUI Hardware & Config Wizard**: A robust, 10-tab terminal-based configurator menu over SSH featuring dedicated `"Hardware Settings"` and `"MQTT Integration"` menus to dynamically configure all frame variables.
- **Config Clamping Safety**: Implements 10 strict boundary checks and clamp safety validation logic inside the TOML configuration loader to guarantee system resilience.

---

## ⚙️ Technical Architecture

```mermaid
graph TD
    A[Media Root] --> B[Phase 1: Recursive Scan]
    B --> C[Phase 2: SQLite Metadata Cache]
    C --> D[Phase 3: Async Preload Pipeline]
    D --> E[SDL3 Render Loop]
    E --> F[DRM/KMS Framebuffer]
    
    subgraph "Preload Pipeline"
    D1[Current Image]
    D2[Next Image]
    D3[Buffer Image]
    end
    
    subgraph "Control HUD"
    H1[Lightweight MQTT Subprocess] -.-> E
    H2[HTTP API Web Remote] -.-> E
    end
```

- **Language**: C++17
- **Core Libraries**: SDL3, SDL3_image, SDL3_ttf, libmpv, SQLite3, libexif, libheif, `mosquitto-clients`.
- **Hardware Accel**: Pi 4/5 VC4 DRM/KMS via SDL3 rendering.

## 📁 Project Structure
```
src/
├── main.cpp          — Entry point, event loop, DRM master flow, motion cooldown
├── mqtt.cpp/h        — Background MQTT subprocess subscriber & HA discovery [NEW]
├── scanner.cpp/h     — Recursive media scanning (getdents64)
├── cache.cpp/h       — SQLite3 WAL-mode metadata persistence
├── config.cpp/h      — TOML config parser & boundary validation
├── tui.cpp/h         — Terminal-based setup & configuration wizard (10 categories)
├── preload.cpp/h     — Two-phase async preload (surface → texture upload)
├── renderer.cpp/h    — SDL_Renderer primitives, EXIF rotation, bias lighting, CRT vignette
├── overlay.cpp/h     — OSD widgets (date, filename, count, timer, clock)
├── transition.cpp/h  — High-performance SDL3 transitions (crossfade, wipe, pixelate, dissolve)
├── mpv_player.cpp/h  — mpv subprocess controller (drmDropMaster/drmSetMaster)
├── image_loader.cpp/h — IMG_Load wrapper (JPEG, PNG, TIFF, WebP, HEIC)
├── font_render.cpp/h  — TTF_RenderUTF8_Blended glow text
├── media_item.h      — Photo and video data structures
├── util.cpp/h         — String parsing, safety, and path utilities
├── fonts/             — Monospace layout fonts (DejaVuSansMono-Bold.ttf)
├── config.toml        — Default config template
└── splash.png         — Boot splash screen image
```
- `install.sh`: Bootstrap installer (sets up Docker Compose plugins, NAS mounts, and systemd units).
- `CHANGELOG.md`: Detailed version history.

---
MIT License
