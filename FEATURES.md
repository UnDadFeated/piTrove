# piTrove Feature Catalog

This document provides a comprehensive, organized overview of all features and capabilities in **piTrove**, a professional-grade, containerized digital picture & video frame application for the Raspberry Pi 4 and 5.

---

## 🏗️ 1. Containerized Deployment & System Architecture
* **Multi-Stage Docker Containerization:** Built on a lightweight runtime image using a multi-stage Debian Trixie Dockerfile, aligning the container's graphics libraries with the host OS.
* **Compose Orchestration:** Orchestrated via `docker-compose.yml` to coordinate persistent host directories, device mounts, runtime variables, and system settings.
* **Headless DRM/KMS Framebuffer:** Operates directly on the GPU framebuffer via DRM/KMS. Runs completely headless without the overhead of X11, Wayland, or a desktop environment.
* **Dynamic GPU & Display Auto-Probing:** Automatically scans system directory entries (`/sys/class/drm/card*-*/status`) on boot to detect active HDMI outputs and inject stable `SDL_VIDEO_KMSDRM_DEVICE` and `SDL_KMSDRM_DEVICE_INDEX` configurations.
* **Systemd Service Daemon Integration:** Configures a system service (`piTrove.service`) to handle container lifecycles on boot and shutdown.
* **Isolated Persistent Directory Mounts:** Bind-mounts persistent directories from the host filesystem into `/app/` in the container:
  * Cache database (`~/piTrove/cache` ↔ `/app/cache`)
  * Runtime config (`~/piTrove/config` ↔ `/app/config`)
  * Application logs (`~/piTrove/logs` ↔ `/app/logs`)
  * Video subtitle tracks (`~/piTrove/subtitles` ↔ `/app/subtitles`)
  * Media storage (`/mnt/nas` ↔ `/app/media` in read-only mode for safety)

---

## 📂 2. Enterprise-Grade Scanning & Caching Engine
* **SQLite3 Metadata & Cache Persistence:** Stores scanned media items, properties, paths, and caching history in an SQLite database using write-ahead logging (WAL) for concurrent read/write support.
* **Concurreny & Lock Protection:** Utilizes thread safety options `SQLITE_OPEN_FULLMUTEX` and a 5000ms busy timeout (`sqlite3_busy_timeout(5000)`) with `std::lock_guard` across all database methods to prevent segfaults or deadlocks between UI and scan threads.
* **Database Transaction Mutex Safeguard:** Implements database-level mutex locks (`db_mutex`) around `begin_transaction()` and `commit_transaction()` to prevent `SQLITE_BUSY` conflicts.
* **NAS-Optimized Scanner:** Avoids filesystem hang scenarios on network mounts (NFS, SMB, CIFS) by using a specialized POSIX `getdents64` implementation coupled with custom directory check alarms and timeouts.
* **Auto-Reconnect Mount Recovery:** Maintains persistent connections to NAS shares, automatically recovering mount connections after network interruptions.
* **Automatic System File & Metadata Exclusion:** Recursively filters out internal directories (such as Synology `@eaDir` or `@Recycle` directories and `Thumbs.db` files) during traversal to prevent scans from hanging.
* **Active Disk Capacity Auditing:** Monitors cache storage space and automatically pauses synchronizations if free space drops below 50MB (`E403` warning).
* **Write Verification Check:** Verifies write permissions on the cache directory upon startup to ensure mounts aren't restricted to read-only (`E405` warning).

---

## 📅 3. Temporal (Seasonal) Slide Windowing
* **Year-Agnostic Month Filter:** Analyzes directory names matching date formats (e.g. `YYYY-MM` or `YYYY_MM`) using regular expressions and displays media matching the current calendar month ± a configurable spread (`window_days`, default 15).
* **High-Precision Date Math:** Tracks seasonal windows using a precise cumulative day-of-year table rather than coarse division, avoiding boundary drift.
* **Season-Neutral Media Support:** Automatically marks folders without date strings (like "Favorites" or general folders) as season-neutral, allowing them to remain in rotation year-round.
* **EXIF and File Attribute Fallback:** Inspects embedded camera capture dates (EXIF) and file creation/modification times for photos without folder-based date prefixes, aligning them automatically with the seasonal window.
* **Midnight Media Rescanning:** Triggers a background scan at midnight to shift the active temporal window dynamically without restarting the application.

---

## 🏷️ 4. Media Classification & Smart Content Filtering
* **Deterministic Keyword & Path Classifier:** Analyzes folder/file structures and camera roll hash distributions (90/10 split) to index media content without heavy machine-learning processes.
* **Auto-Filter Clutter:** Identifies and filters out non-photographic images such as screenshots, documents, graphics, receipts, spreadsheets, logos, and icons by default.
* **Keep People Filter:** Automatically parses keywords and metadata to prioritize photos of family, friends, portraits, and travel.
* **Keep Animals Filter:** Detects and retains photos of pets, wildlife, and animals.
* **Optical EXIF Verification:** Differentiates camera-captured photos from high-fidelity phone screenshots by checking for optical camera tags (such as `ExposureTime`, `FNumber`, `ISOSpeedRatings`, `FocalLength`).

---

## 🎨 5. Advanced SDL3 Rendering Pipeline
* **SDL3 Core Pipeline:** Leverages SDL3 rendering routines (`SDL_RenderTexture`), floating-point layout structures (`SDL_FRect`), and native texture querying APIs for fast graphics rendering.
* **GLES2 / GLES3 Texture Alignment:** Converts HEIC files from RGB to RGBA format to ensure correct graphics memory alignment and GLES2/GLES3 compatibility.
* **Ken Burns Effect:** Custom panning and zooming animations that smoothly crossfade and scale back to center (1.0x) at transition completion to prevent size snap and jitter.
* **Twin-Portrait Split Collage Layout:** Automatically groups adjacent portrait-format photos into a side-by-side split screen. Includes split borders, layout filenames, and double-advance seek capabilities.
* **Edge-to-Edge Color-Matched Matte Background:** Extends a color-matched average background to fill the screen, eliminating static black borders around photos.
* **Procedural Ambient Matte Patterns:** Generates moving lines and undulating wave patterns that match the average color of the active image. Includes selectable background pattern geometries (triangles, polygons, squares, rectangles, hexagons, fractals, static/animated blends, and mixed random styles).
* **3D Picture Frame Miter Border:** Renders a wood/metallic-style picture frame border using average color-matched highlights, crease lines, and a 1px black outline.
* **Bias Lighting Edge Glow / 3D Shadow:** 
  * **3D Edge Glow:** Restricts ambient glow to the right and bottom edges to create a 3D drop-shadow effect.
  * **Multi-Stop Sampling:** Samples up to 24 stops per edge (80px wide × 5px deep) to produce smooth color gradients fading into the matte.
  * **Animation Styles:** Toggleable styles including `edge_glow`, `pulse`, `breathe`, and `none`.
* **CRT Display Emulator:** Renders a curved CRT screen vignette and scanline overlays.

---

## 🔄 6. Transition Engine
* **High-Performance Transitions:** Hardware-accelerated GLES3/GLES2 transitions including Crossfade, Wipe, Pixelate, and Dissolve.
* **Smooth Frame-Timer Calibration:** Resets the transition timer at start to skip disk loading lag, preventing transition stutters or instant slide pops.
* **Seamless Buffer Presentation:** Renders final transition frames even after transition flags clear, avoiding single-frame black flickers.

---

## 🎥 7. Native Video Playback (mpv Integration)
* **Accelerated Subprocess Controller:** Spawns a background `mpv` process rendering directly via DRM/KMS (`--vo=drm`). Restores DRM master context cleanly on exit.
* **Hardware-Accelerated Decoding:** Exposes Broadcom stateful V4L2 decoder/encoder hardware blocks (`/dev/video19` through `/dev/video35`) to play high-bitrate HEVC/H.264 video clips seamlessly (including 4K @ 60fps) using `v4l2m2m-copy` and `drm-copy` codecs.
* **Aggressive Network Video Caching:** Configures mpv buffer thresholds (up to 120s buffering, up to 1024MB memory limit, larger I/O read chunks) to prevent buffering stalls over Wi-Fi/NAS.
* **Balanced Video-to-Photo Interleaving:** Interleaves videos at a custom ratio (e.g. 3 videos per 10 photos). Avoids consecutive videos by skipping back-to-back video clips, ensuring a photo separates them.
* **Independent Video Cooldown:** Video files respect the same configurable cooldown duration (default 330 days) as photos.
* **Centralized Subtitle Management:** Automatically scans `/app/subtitles` for matching `.srt` files and loads them. Configured with OSD margin matching the matte size, standard vertical caption position, and EIA-608 closed caption track generation.
* **Video Aspect Ratio Preservation:** Forces `--keepaspect=force` to letterbox/pillarbox videos rather than stretching.
* **Dynamic CPU Core Scaling:** Plays videos using `max_cores - 1` decoding threads to optimize hardware performance.

---

## 🏠 8. Smart Home & MQTT Integration
* **Lightweight Subscriber Pipe:** Spawns a background subprocess listener executing `mosquitto_sub -F "%t:%p"` to receive remote controls instantly and safely with zero rendering loop delay.
* **Home Assistant Auto-Discovery:** Automatically publishes standard JSON config payloads to `homeassistant/` on startup. Instantly registers:
  * **Screen Switch** (Toggles physical backlight and solid black blanking overlay)
  * **Skip Next & Previous Buttons** (Remote slideshow navigation)
  * **Play/Pause Toggle Button** (Remote execution control)
  * **Motion Binary Sensor** (Auto-syncs with physical room motion)
* **Automatic Cooldown Blanking:** Monitors the room's motion sensor topic. If no motion is detected within a customizable cooldown window, it blanks the screen physically using `vcgencmd display_power 0` and clears the framebuffer to black. Wakes up instantly on new motion or key down/mouse events.

---

## ☁️ 9. Google Photos Cloud Sync
* **Background GPhotos Synchronization:** Connects to the Google Photos API to pull down media items from user-selected albums at scheduled intervals.
* **Glassmorphic OAuth Authorization Setup Wizard:** Built-in interactive landing page served at `/google_photos_setup` to configure client credentials, complete OAuth redirect flow, and persist refresh tokens.
* **Security safeguards:** Strict dynamic redirect URL domain validation, SSRF protection, curl qualification to prevent option injection.

---

## 💻 10. Web Remote Dashboard & HTTP Controller
* **Modern Zinc Dashboard & Themes:** Sleek UI with user-selectable Light/Dark modes and accent themes (Zinc, Emerald, Sapphire, Amber).
* **Live Slide Telemetry:** Real-time slideshow status, progression countdown timer, active slide name, and cache database statistics.
* **Real-Time Log Stream Window:** Live log inspection directly in-browser. Feature-rich scroll lock preserves read position as new logs come in. Adjustable font sizing.
* **Dynamic Visual Preview:** Automatically refreshes the active photo preview using cache-busting timestamps and loading animations.
* **Web Control Knobs:** Toggle switches for Touchscreen Mode, Shuffle, Ken Burns, Blurred Background, Matte Opacity, Volume, and more.
* **Security & Reliability:** Limits connections to 32 concurrent dashboards, binds to alternative ports if default port (9000/8080) is taken, and sets socket timeouts to prevent Slowloris resource exhaust.

---

## 📺 11. On-Screen Display (OSD) & Direct-to-Device Interfaces
* **Continuous OSD Rendering:** Overlays info HUD, clock, filename, slide counter, countdown timer, seasonal anniversary banners, and diagnostic alert boxes directly over active transition frames.
* **Adaptive Contrast Text Colors:** Dynamically evaluates brightness at screen boundaries/edges of images to adjust OSD text foreground (white/black) and shadow/glow layers.
* **Gold Ribbon Anniversary Badge:** Renders on slides when matching the exact anniversary date (month and day).
* **Direct mouse menu:** Right-clicking connected mouse pops up direct settings menu overlay.
* **Touchscreen Interface Mode:** Touching display shows quick control overlays (prev/pause/next/settings). Features on-screen increment/decrement buttons, slider widgets, and an interactive virtual numeric keyboard overlay.
* **Dynamic Touchscreen Hotplugging:** Automatically re-detects and binds touchscreens reconnected at runtime.

---

## 🛡️ 12. Deployment, Diagnostics & Fallback Safety
* **Advisory Process Locking:** Employs POSIX `flock(LOCK_EX | LOCK_NB)` on a PID lock file (`piTrove.lock`) to prevent concurrent instances, with automatic recovery via `kill(pid, 0)` liveness checks.
* **Double-Buffered Async Logger:** Relies on an asynchronous logging system with a background flush thread and double-buffering, minimizing I/O blocking on the main thread during high disk load. Keeps logs clean with a maximum retention of the 5 most recent files.
* **Centralized Diagnostic Log System:** Unified error handling displaying glowing phosphor-red alert boxes with unique 4-digit code catalogs (`E###`). Spans across network/storage, media decoding, cloud, SQLite, system hardware, graphics pipeline, MQTT, and config categories.
* **Hardware Telemetry Monitor:** Tracks SoC chip core temperature, triggering overheating warnings (`E501`) above 80°C.
* **Active Software Watchdog:** Monitor thread automatically restarts slideshow loop if it remains frozen or locked for more than 45 seconds.
* **Native Network Link Monitor:** Diagnostic thread queries connection status and runs socket reset operations automatically on gateway loss, replacing old external cron scripts.
* **Configuration Boundary Hardening:** Standardized TOML syntax parsing audits (`E801`) and clamping to safety limits.
* **Graceful Signal Management:** Registers `SIGINT`/`SIGTERM` to restore physical display backlight power (`vcgencmd display_power 1`) and safely close SQLite connections on exit.
* **Network & SSH Resilience Tuning:** Installer overrides NetworkManager to disable Wi-Fi Power-Saving persistently (`wifi.powersave = 2`) and configures server-side SSH/SFTP keepalives (60-second intervals) to avoid dropouts.
* **Integrated Media Organizer Utility:** Built-in compiled utility run via `--organize` CLI flag to chronologically group folders or apply date-prefixes in-place.
* **Automated Daily Updater Scheduler:** Command-line options (`install.sh --update --cron`) to check the chosen Git branch (stable `main` or active `develop`) and safely merge new configurations.
* **Proprietary Codec Support:** Installer downloads `libavcodec-extra` to supply H.264/HEVC support on fresh Debian Trixie installations.
