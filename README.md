# piTrove

> A Raspberry Pi digital picture frame — robust media loading, CIFS/NFS-safe scanning, dynamic ambient lighting, and a CRT-themed splash UI.

[![Platform](https://img.shields.io/badge/platform-Pi%204%20%7C%20Pi%205-blue?style=flat-square)](https://www.raspberrypi.com/)
[![OS](https://img.shields.io/badge/OS-Trixie%20Lite%20%28Debian%2013%29-lightgreen?style=flat-square)](https://www.debian.org/)
[![Architecture](https://img.shields.io/badge/arch-aarch64-orange?style=flat-square)](https://en.wikipedia.org/wiki/AArch64)
[![Graphics](https://img.shields.io/badge/graphics-DRM%2FKMS%20%7C%20GLES2-red?style=flat-square)](https://www.mesa3d.org/)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)
[![raylib](https://img.shields.io/badge/raylib-6.x-purple?style=flat-square)](https://www.raylib.com/)

## Quick Start

### 1. Flash the OS

1. Download [Raspberry Pi Imager](https://www.raspberrypi.com/software/)
2. Open it, choose your Pi model
3. **Choose OS** → Raspberry Pi OS (other) → **Raspberry Pi OS Lite (64-bit)**
4. Select your SD card and flash

> ⚠️ **Must be 64-bit** (not 32-bit). The app requires aarch64.

### 2. Boot and SSH

1. Insert the SD card, boot the Pi
2. Connect to your network, find the Pi's IP
3. SSH in: `ssh pi@<pi-ip>` (password: `raspberry`)

### 3. Install piTrove

```bash
wget -qO- https://raw.githubusercontent.com/UnDadFeated/piTrove/main/install.sh | sudo bash
```

That's it — install, build, configure, and run.

## Features

### Media Engine

| Feature | Details |
|---------|---------|
| **Multi-format** | JPEG, PNG, TIFF, WebP, HEIC/HEIF, BMP, TGA |
| **Robust loading** | stb_image → format-specific loaders → skip on failure |
| **EXIF rotation** | Auto-orientation via libexif |
| **Video playback** | mpv DRM with HEVC (h.265) / H.264 |
| **Corrupted files** | Automatic skip, no hangs on NFS/CIFS |
| **Max resolution** | Images >1920px auto-resized via NEON-accelerated ImageResize |

### Scanning & Cache

| Feature | Details |
|---------|---------|
| **CIFS-safe scan** | `getdents64` with timeout wrappers, no NFS hangs |
| **Multi-threaded** | Producer/consumer with configurable concurrency |
| **SQLite3 cache** | WAL mode, 64MB mmap, auto-corruption tracking |
| **Temporal window** | Show only files from last N days (default: 15) |
| **Cooldown** | Skip photos shown within N days (default: 330) |
| **Recursive scan** | Configurable depth (default: 10 levels) |
| **Ignore folders** | Skip directories via `ignore_folders` config |

### Slideshow

| Feature | Details |
|---------|---------|
| **Transitions** | Crossfade, wipe, pixelate |
| **Ken Burns** | Configurable zoom animation with speed & amount |
| **Interleaved** | Videos mixed with photos at configurable ratio |
| **Dynamic ambient** | Photo-aware edge glow, radial orbs, bias lighting |
| **3D frame** | Dynamic highlight/shadow derived from photo color |
| **Collage mode** | Multi-photo grid layout (configurable cols/rows) |
| **Shuffle** | Randomized slide order |
| **Vignette** | CRT screen curvature vignette overlay |

### UI & Overlays

| Feature | Details |
|---------|---------|
| **CRT splash** | Terminal-themed progress with scanlines, neon bar |
| **Telemetry** | CPU temp, clock speed, elapsed time during scan |
| **Timer overlay** | Photo countdown + video remaining time |
| **Date display** | Configurable text, position, color (EXIF or clock) |
| **Clock** | 12h/24h time display, configurable position & color |
| **Filename** | Bottom-left file name overlay |
| **Counter** | "5 / 24380" photo count display |
| **Custom fonts** | Configurable font sizes for all overlays |

### System

| Feature | Details |
|---------|---------|
| **DRM/KMS** | Native framebuffer — no X11 or Wayland needed |
| **GLES2** | OpenGL ES 2.0 via raylib — optimized for Pi GPU |
| **Systemd service** | Auto-start, auto-restart, journal logging |
| **Single instance** | flock-based PID locking |
| **HTTP remote** | `/api/next`, `/api/prev`, `/api/pause` endpoints |
| **SSH-safe TUI** | Adaptive 4-column config wizard works over SSH |
| **Display off** | Scheduled sleep/wake times for power saving |
| **Brightness auto** | Auto backlight dimming based on time of day |
| **Touch input** | Tap left=back, right=forward (when enabled) |
| **Auto rotation** | Auto-rotate based on image EXIF orientation |
| **Weather** | Optional temperature/weather overlay |

## Configuration

Config file: `src/config/config.toml`

### Key Settings

| Section | Key | Default | Description |
|---------|-----|---------|-------------|
| `slideshow` | `transition_delay` | `120.0` | Seconds per photo |
| | `transition_duration` | `1.5` | Transition time |
| | `transition_effect` | `crossfade` | crossfade / wipe / pixelate |
| | `ken_burns` | `0` | Enable Ken Burns zoom |
| | `ken_burns_speed` | `0.1` | Zoom animation speed |
| | `ken_burns_zoom` | `0.15` | Zoom amount |
| | `matting` | `1` | Show border around photos |
| | `matting_size` | `48` | Border width in pixels |
| | `bias_lighting` | `1` | Ambient bias lighting on/off |
| | `bias_anim_style` | `edge_glow` | pulsing / radiating / absorbing / edge_glow |
| | `bias_anim_speed` | `0.5` | Animation speed (0.0–5.0) |
| | `bias_color_mode` | `auto` | auto / rainbow |
| | `bias_strength` | `110` | Glow intensity |
| `scan` | `window_days` | `15` | Only show files from last N days (0 = all) |
| | `cooldown_days` | `330` | Min days between showing same photo |
| | `recursive` | `1` | Recursive directory scan |
| | `scan_depth` | `10` | Max recursion depth |
| | `max_concurrent` | `4` | Thread pool size |
| `overlay` | `timer_enabled` | `1` | Show countdown timer |
| | `filename_enabled` | `1` | Show filename overlay |
| | `count_enabled` | `0` | Show "5 / 24380" count |
| | `videos_per_photos` | `3` | Video/photo interleaving ratio (0=disabled) |
| | `sleep_time` | `""` | Display off time (HH:MM) |
| | `wake_time` | `""` | Display on time (HH:MM) |
| `date_overlay` | `enabled` | `0` | Show date overlay |
| | `text` | `"%Y-%m-%d"` | Date format string |
| `display` | `border_enabled` | `1` | 3D frame border on/off |
| | `border_width` | `10` | Border thickness in pixels |
| | `vignette_enabled` | `1` | CRT vignette overlay |
| | `shuffle` | `0` | Randomize slide order |
| | `auto_display_rotation` | `0` | Auto-rotate based on EXIF |
| `clock` | `clock_enabled` | `0` | Show clock overlay |
| | `clock_24h` | `1` | 24-hour format |
| `brightness` | `brightness_auto` | `0` | Auto backlight dimming |
| | `brightness_auto_min` | `50` | Minimum brightness % |
| | `brightness_auto_max` | `100` | Maximum brightness % |
| `collage` | `collage_enabled` | `0` | Multi-photo grid mode |
| | `cols` | `2` | Grid columns |
| | `rows` | `2` | Grid rows |
| `touch` | `touch_enabled` | `0` | Touch input support |
| `weather` | `weather_enabled` | `0` | Weather overlay |
| | `weather_lat` | `-999.0` | Latitude |
| | `weather_lon` | `-999.0` | Longitude |
| `remote` | `http_enabled` | `0` | HTTP remote API |
| | `http_port` | `8080` | API port |
| `paths` | `media_dir` | `/mnt/nas` | Root media directory |
| | `cache_dir` | `~/.cache/piTrove` | SQLite cache location |

## Architecture

```
Scan Pipeline:
  Phase 1 ── Multi-threaded directory scan (getdents64, CIFS timeout)
     ↓
  Phase 2 ── SQLite cache building (EXIF, dimensions, duration)
     ↓
  Phase 3 ── Slideshow loop (async preload, transitions, bias lighting)

Image Loading:
  stb_image (raylib) → format-specific loader → skip on failure
  JPEG → libjpeg-turbo | PNG → libpng | TIFF → libtiff
  WebP → libwebp | HEIC → libheif | BMP/TGA → stb_image

Preload Pipeline:
  Start ──> first_img_thread (instant startup image)
          └─> preload_thread (background images 1–N)
             └─> preload_next (rolling window, 2 images ahead)
```

## Project Layout

```
/home/pi/piTrove/
├── src/
│   ├── piTrove.cpp           ← All source (~4800 lines)
│   ├── CMakeLists.txt        ← Build config (DRM/GLES2)
│   ├── config/config.toml    ← App configuration (generated by install.sh)
│   ├── fonts/                ← CRT overlay font
│   ├── splash.png            ← Splash screen image
│   └── build/                ← CMake build output
├── piTrove/                  ← Built binary (not tracked in git)
├── install.sh                ← Bootstrap installer
├── README.md                 ← This file
├── CHANGELOG.md              ← Version history
├── logs/                     ← Runtime logs
└── ~/.cache/piTrove/         ← SQLite cache.db (WAL mode)
```

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for full release history.

---

MIT License
