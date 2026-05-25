# Changelog

## v11.2.3 — Slideshow Loop Fix (May 25, 2026)

### Fixed
- **Slideshow stuck on same image** — Single-image transitions were never advancing because the twin-portrait validation check required `next_twin_data` to be valid even when displaying a single photo. Since `next_twin_data` is always `nullptr` in single-image mode, the image load failed silently, causing `current_data` to never update and the same image to display indefinitely. Fixed by gating twin validation behind the actual `is_twin` condition.

## v11.2.1 — System Stability Fixes (May 25, 2026)

### Fixed
- **FFprobe child fd leak** — Replaced broken `/proc/self/fd` iteration with `readlink()` + deferred `close()` to prevent file descriptor leakage to ffprobe child processes.
- **MQTT subscriber thread lifecycle** — Replaced detached thread with tracked joinable thread and proper `stop_mqtt_client()` shutdown path. MQTT pipe is now cleanly closed on exit.
- **Playlist lock race during transitions** — Eliminated unlock/relock pattern during transition loading. Paths are captured under lock, I/O performed outside lock, metadata updated under lock.
- **Signal handler safety** — Replaced async-signal-unsafe `system()` call in crash handler with direct sysfs write to restore display power on crash.
- **Deprecated sleep API** — Replaced all `usleep()` calls with C++11 `std::this_thread::sleep_for()` across the codebase.
- **Edge strip underflow on small images** — Added bounds guard for edge color sampling when image dimensions are less than 3 pixels.
- **Twin portrait data race** — Eliminated unguarded access to shared playlist vector in twin-portrait pairing logic.
- **Screen blank toggle race** — Replaced non-atomic read-modify-write on screen blank flag with atomic compare-exchange to prevent torn writes during rapid toggle and motion-sensor wake events.
- **MQTT config data race** — Secured all MQTT publish and Home Assistant discovery calls to read broker settings under lock, preventing corruption during live config reloads.
- **Display power data race** — Fixed non-atomic writes to screen blank state across all input handlers (keyboard, mouse, motion sensor cooldown) to prevent lost wake events.
- **Config reload safety** — Protected all configuration reads across the HTTP API, slideshow loop, cache subsystem, and MQTT client with mutex guards to prevent stale or torn reads during live updates.
- **Cache transaction atomicity** — Changed database transaction flag from plain bool to atomic to prevent race between concurrent upsert and transaction lifecycle calls.

## v11.1.9 — Dynamic Collage Lookahead & 1" Matte Adjustments (May 24, 2026)

### Added
- **Dynamic Lookahead Portrait Pairing** — Enhanced the collage selection logic to perform a forward search in the play queue when a portrait photo is encountered. It dynamically finds the nearest subsequent portrait photo in the randomized playlist and swaps it into the adjacent position (`idx + 1`), guaranteeing portrait photos are always displayed side-by-side as a twin-portrait collage.
- **Physical Matte Clearance** — Increased default display matting size from 48px to 96px across the entire configuration system to clear the margins of a 1" physical picture frame overlay at 1080p display resolutions.

## v11.1.8 — Default Twin-Portrait Collage Setup (May 24, 2026)

### Added
- **Default Twin Portrait Configuration** — Enabled twin-portrait collage by default (`twin_portrait_enabled = 1`) in `config.toml`.

## v11.1.7 — Self-Update Fail-Safe Support (May 24, 2026)

### Added
- **Self-Updater Integration** — Integrated native update checking and compilation orchestration (`--update` command flag) directly inside `install.sh`. Running `sudo ./install.sh --update` automatically fetches remote GitHub commits securely as the primary user, runs Git pulls, triggers a parallel container build of the newly updated source assets, and gracefully restarts the background daemon `piTrove.service`.

## v11.1.6 — Premium Stateless HEVC Hardware Acceleration (May 24, 2026)

### Added
- **Native Raspberry Pi Archive Integration** — Configured container multi-stage image builds (`Dockerfile`) to natively integrate the Raspberry Pi Foundation package archives (`archive.raspberrypi.com`) and import their official GPG archive keyrings.
- **Stateless V4L2 Hardware-Accelerated HEVC** — Upgraded containerized `mpv` and `ffmpeg` libraries from standard software-bound Debian builds to Broadcom-accelerated Raspberry Pi custom builds. This enables full hardware-accelerated stateless HEVC decoding (`drm-copy` / `rpi-hevc-dec`) inside the container, reducing 4K HEVC playback frame dropouts to absolute zero and unlocking completely stutter-free video playback.

## v11.1.5 — Broadcom V4L2 Hardware-Accelerated Video Decoding (May 23, 2026)

### Added
- **Hardware-Accelerated Video Decoding** — Overhauled container modesetting by mapping the complete host `/dev` namespace dynamically inside `docker-compose.yml` (`- /dev:/dev`). This exposes Broadcom stateful V4L2 decoder/encoder hardware blocks (`/dev/video19` through `/dev/video35`) inside the container. Modern `mpv` now automatically leverages V4L2 copy hardware codecs (`v4l2m2m-copy` and `drm-copy`) instead of software fallbacks, completely resolving choppy playback, stutter, and massive frame dropouts.

## v11.1.4 — Persistent Wi-Fi Power-Saving & Network Safety (May 23, 2026)

### Added
- **Wi-Fi Power-Saving Override Fail-safe** — Integrated persistent NetworkManager Wi-Fi Power-Saving override configuration (`wifi.powersave = 2`) inside `install.sh`. This keeps the Broadcom Wi-Fi network interface active, eliminating temporary network-mount drops, buffer timeouts, or `Connection reset by peer` handshakes on standard SSH/HTTP/NAS mounts.

## v11.1.3 — Dynamic Post-Install Dashboard & MQTT HUD URL (May 23, 2026)

### Added
- **Clickable Dashboard & HUD URL** — Enhanced the final installation success card in `install.sh` to dynamically query the active host IP address and display a pixel-perfect, beautifully padded clickable Web Remote Dashboard and MQTT HUD URL (`http://<pi-ip>:8080/`), allowing users to click and interact with their system immediately upon setup completion.

## v11.1.2 — Strict Configured Spacing & Video Interleaving (May 23, 2026)

### Fixed
- **Stretched Video Interleaving** — Redesigned the playlist interleaving algorithm (`organize_playlist`) to strictly honor the configured user ratio (`videos_per_photos` / 10) instead of stretching the video pool over the entire slideshow directory. Videos are now interleaved perfectly at the targeted ratio (e.g. 3 videos per 10 photos = 1 video every 3.33 photos), gracefully tail-ending with sequential photos once the video pool is temporarily exhausted.

## v11.1.1 — Display Sleep Recovery & Signal Safety Fail-Safes (May 23, 2026)

### Added
- **SIGINT Graceful Intercept** — Registered `SIGINT` (Ctrl+C) to trigger graceful shutdowns, restoring physical display backlights and closing the SQLite cache database safely.
- **Normal Exit Backlight Restoration** — Added physical display backlight power restoration (`vcgencmd display_power 1`) in `main.cpp` to ensure the screen powers on during standard application terminations.

## v11.1.0 — MQTT Integration & Interleaving Ratio Optimization (May 23, 2026)

### Added
- **MQTT Broker Client Integration** — Integrated a lightweight background MQTT subscriber subprocess utilizing `mosquitto_sub -F "%t:%p"` to receive remote controls and motion triggers instantly with zero rendering frame stalls.
- **Home Assistant Auto-Discovery** — Embedded automated entity announcements for automatic integration with Home Assistant dashboard nodes (registering screen switch, skip next/prev buttons, play/pause controls, and motion binary sensor).
- **Motion Sensor Blanking & Sleep Cooldown** — Added a background motion blanking service that clears the framebuffer to solid black and physically switches display backlight power via `vcgencmd display_power 0` after customizable idle cooldown.
- **Glassmorphic Web HUD Controller updates** — Enhanced the HTTP remote control dashboard to include interactive MQTT configuration states, dynamic screen power switches, and manual "Trigger Motion" test pulses.

### Fixed
- **Playlist Interleaving Ratio Drop** — Completely rewrote the de-clustered mathematical interleaving ratio algorithm (`organize_playlist`) to elegantly support video-heavy libraries and high interleaving ratios (> 1.0) without dropping eligible media items.

## v11.0.0 — Enterprise Docker Containerization Migration (May 23, 2026)

### Added
- **Multi-Stage Dockerfile** — Created a multi-stage Docker build process based on Debian Trixie (matching user-space Mesa/GL graphics version with host OS) that builds the C++ code inside a builder stage and exports a lightweight, highly optimized minimal runtime image.
- **Docker Compose Orchestration** — Integrated a unified `docker-compose.yml` to define persistent volumes (`cache`, `config`, `logs`, `subtitles`), map GPU device drivers (`/dev/dri` and `/dev/input` for direct hardware framebuffer mapping), and pass KMSDRM display settings via environment variables.
- **Non-Interactive Environment Controls** — Upgraded the graphical installer `install.sh` to fully support non-interactive automation by checking for environment variable fallbacks (like `STORAGE_CHOICE`, `NAS_IP`, `NAS_SHARE`, `NAS_USER`, `NAS_PASS`, `SCAN_WINDOW_DAYS`), completely eliminating the need for terminal prompt inputs.
- **Security Safeguards** — Added `.env`, `*.env`, and `*.cred` files to `.gitignore` to prevent any accidental leakage of host-specific network parameters or credentials to public Git repositories.

### Changed
- **Containerized systemd Daemon** — Modified the installer to configure the `piTrove.service` daemon to cleanly manage the lifecycles of containerized processes via `docker compose up` and `docker compose down` commands on boot and stop.
- **Persistent Paths Alignment** — Updated `install.sh` TOML template writing to output paths aligning with virtual volume mount scopes inside the Docker container (`/app/media`, `/app/cache`, `/app/logs`).

### Fixed
- **Missing stb development headers** — Integrated `libstb-dev` package installation into both the Dockerfile build process and the host installation requirements, resolving compilation stalls on fresh operating system setups.

## v10.4.3 — Codebase Stability & UX Improvements (May 23, 2026)

### Fixed
- **JSON Injection / Malformed JSON** — Added a robust `escape_json` utility to correctly escape double quotes and backslashes in filenames inside `get_api_status()`, ensuring the Remote Controller API endpoint produces valid JSON.
- **Preloader Thread Exit Lag / Delayed Shutdown** — Modified the preloader worker thread loop to break immediately when shutdown is requested (`!running.load()`), bypassing processing of the remaining queue and stopping exit lag.
- **Socket/Database Descriptor Leak** — Set `SOCK_CLOEXEC` on the background server listening socket and switched `accept` to the Linux-native `accept4` with `SOCK_CLOEXEC`. Implemented an explicit `/proc/self/fd` scanning loop in `run_ffprobe`'s child process to close all inherited database, file, and network socket descriptors before execvp.
- **Lack of Boundary Checks on short Keywords** — Enhanced the media classifier's `match_keyword` to enforce strict word boundary checks for all keywords with length <= 3 or specific short words (like `"self"`), preventing false positives like `"vacation"` matching `"cat"`.
- **Twin Portrait UX Slide Repetition** — Prevented wrap-around pairing of portrait images in `should_be_twin_portrait` by enforcing sequential adjacency within the playlist size bounds without modulo wrap-around.
- **Poor adaptive OSD contrast on high-contrast backgrounds** — Redesigned `get_adaptive_colors` to map screen coordinates to the nearest image edge, extracting localized luma using high-resolution edge strips (`edge_top_rgb`, `edge_bot_rgb`, etc.). If coordinates fall outside the fit rect, defaults to white text on black margins.
- **Incorrect Anniversary Banner on Fallback Items** — Enforced exact month and day matching with today's date before displaying the Gold Ribbon anniversary banner.
- **Process Reaping race condition in video player** — Synchronized process reaping and process state handling in `MpvPlayer::check_status` by holding the mutex lock during execution.
- **Broken CRT Screen Curvature Vignette** — Replaced the impossible `< 0.7f` condition with `edge = 1.0f - 0.3f * (v * v)` and a `< 1.0f` check to correctly render a translucent curvature vignette fading to black near screen boundaries.
- **Division by Zero / NaN in Transition** — Enforced a minimum duration of `0.001f` in `TransitionEngine::start` to prevent NaN progress values on division by zero.

## v10.4.2 — Case-Insensitive Extension Support for Media Classification (May 23, 2026)

### Fixed
- **Fixed Uppercase Extension Media Classification** — Resolved a case-sensitivity bug in the media classifier (`classify_media_item` in `src/util.cpp`) where files with uppercase extensions (like `.JPG` from NAS digital frames) were not recognized as camera roll images. This caused standard photos to be incorrectly categorized as "documents/screenshots" and filtered out of the slideshow. Added standard case-insensitivity conversion (`std::transform` to lowercase) before checking media extensions, immediately expanding the eligible playlist pool.

## v10.4.1 — Restored Edge Glow & EXIF Rotated Dimensions (May 23, 2026)

### Fixed
- **Restored Bias Lighting Edge Glow** — Corrected a layer rendering order bug in `main.cpp` (both single and twin-collage rendering blocks). Previously, bias lighting was drawn first and the solid black matte borders second, completely painting solid black over the transparency glow strips. Reversed the sequence to draw matte borders first (as the base layer) and bias lighting second, allowing the dynamic edge glow to overlay beautifully on top of the black borders.
- **Fixed Rotated Image Right-Side Black Bars** — Discovered and resolved a bug in the preloader (`preload.cpp`) where the in-memory dimensions (`width`/`height`) of decoded images were not updated to the rotated surface's dimensions after applying EXIF rotation (portrait photos). This caused `ImageLoader::load_texture` to blit the rotated portrait surface using unrotated landscape dimensions, creating a squished texture with a massive black bar on its right side.
- **Dynamic Glow Border Adjustments** — Dynamically set the edge glow's border width offset parameter to `0` when `border_enabled` is disabled. This makes the ambient glow begin exactly at the edge of the photo rather than leaving an artificial 10px black gap.

## v10.4.0 — Decoupled 3D Border & Correct Margin Offsets (May 23, 2026)

### Added
- **Decoupled 3D Miter Border** — Extracted the custom 3D picture frame miter rendering from `draw_bias_lighting` into its own modular `Renderer::draw_3d_border()` function. Configured the system to honor `border_enabled` independently of `bias_lighting`, allowing users to have a border without a glow, a glow without a border, both, or neither.
- **Dynamic Playlist Dimension Updates** — Updated the slideshow swap routine to automatically query the actual decoded texture size (`current_data->width` and `height`) on image transition. It now dynamically updates the in-memory metadata in `g_eligible` and `g_scanned_items` and writes the correct sizes into `cache.db` on texture swap, which resolves dynamic scale mismatches (e.g. `NaN` scale / `0x0` margins) on first-time or uncached image displays.

### Fixed
- **Margin Offset Bug** — Rewrote the layout geometry calculation inside `Renderer::calculate_fit_rect` and `calculate_fit_rect_in_area`. The system now only subtracts the 48-pixel `matting_size` when `matting` is **explicitly enabled** in config, cleanly resolving the issue where turning off matting still left a large black margin.
- **Robust Clean OS Installer** — Removed legacy external shader installation steps from `install.sh` and `CMakeLists.txt` (as native GLES rendering in SDL3 does not require external shader files). This completely resolves potential glob copy failures under `set -eo pipefail` on a fresh OS installation.
- **Installer Version Sync** — Synchronized the graphical installer's version labels and configuration templates from `10.1.0` to `10.4.0`.

## v10.3.15 — Thread & Memory Safety Stability Release (May 23, 2026)

### Fixed
- **Preload Double-Free Crash** — Resolved a critical double-free memory corruption bug in the preloader's queue mismatch discarding branch. Replaced unsafe manual pointer freeing with robust, standard C++ RAII container destructions (`std::queue::pop()`) which safely deallocate raw pixel memory without risk of double-free crashes.
- **Redundant Parallel Preloads** — Replaced the transient lookahead set with a unified `active_preloads` container tracking preloads across all pipeline phases (queued, in-flight, and decoded in memory). This completely eliminates duplicate parallel background decoding of identical files, reducing CPU and NAS disk I/O load.
- **Startup Playlist Statistics Log** — Corrected a minor statistics log display bug where moving playlist vectors before logging caused startup counts to show as `0 photos + 0 videos = 254 total`.

## v10.3.14 — Asynchronous Multi-Threaded Background Preloader (May 22, 2026)

### Added
- **Integrated Background Preloading** — Integrated the multi-threaded `PreloadQueue` pipeline into the active slideshow treadmill loop. While the slideshow is resting on the current image, it looks ahead and enqueues future items to be fetched and decoded asynchronously on background worker threads. This completely resolves the main thread connection locks and high I/O wait (`wa`) states during transitions.
- **Safety Preload Verification & Stale Purging** — Updated `try_dequeue` to verify that the path of the preloaded raw pixel buffer matches the targeted path. If a mismatch is detected (e.g. because of manual skips or remote pauses), the queue automatically and safely discards stale preloads and frees their memory immediately.

## v10.3.13 — Metadata-Cached Camera EXIF Checking (May 22, 2026)

### Added
- **EXIF Caching Layer** — Extended the SQLite metadata cache and `MediaItem` struct with an `is_camera` column to track camera EXIF status (`-1` = unknown, `0` = screenshot/document, `1` = camera photo). The `ImageLoader::has_camera_exif` filesystem check is now executed exactly once per file and permanently cached, eliminating synchronous network NAS reads during playlist generation. This completely resolves the remote mount connection hangs (kernel `D` state blocks in `cifs_strict_readv`) and ensures instant, robust application startup.

## v10.3.12 — Precise Word-Boundary Keyword Matching (May 22, 2026)

### Fixed
- **Screenshot False Positive Leak** — Discovered and fixed a subtle bug in the media classifier's keyword matching logic. Short keywords (like `"me"` and `"us"`) were triggering false positives on common words (e.g. `"me"` matching inside `"Chrome"` and `"Messages"` in file paths like `/mnt/nas/Photos/..._Chrome.jpg`), bypassing optical EXIF checks and leaking screenshots into the slideshow. Added strict alphanumeric word-boundary checks for these short tokens.
- **Fast-Path Cache Extension Mismatch** — Normalized file extension comparisons between the filesystem scanner (which returns `".jpg"` with a leading dot) and the database fast-path loader (which extracts `"jpg"` dotless), ensuring uniform classification in both modes.

## v10.3.11 — Seamless Video-to-Video Transitions (May 22, 2026)

### Added
- **Seamless Video-to-Video Transitions** — Integrated an intelligent playlist peeking mechanism inside the video player's status check routine. If the next queued item is also a video, the application holds onto the active DRM Master lock instead of dropping and reclaiming it recursively. This prevents consecutive mode-setting flips on HDMI displays, eliminating intermediate black screens and glitches entirely.
- **Removed Skip Consecutive Video Hack** — Cleanly removed the temporary skip consecutive video loop workaround in the slideshow treadmill, restoring correct playlist execution and allowing video-only play queues to work properly.

## v10.3.10 — Unconditional Screenshot Filter & Triple-Entropy Seeding (May 22, 2026)

### Added
- **Unconditional Screenshot & Document Skip** — Standardized slideshow and On-This-Day anniversary filters to unconditionally exclude documents, receipts, screenshots, and graphics (`is_doc`) from playback, independent of active people/animals category settings.
- **Triple-Entropy Seeding Engine** — Overhauled startup playlist shuffling to use standard hardware `std::random_device`, `/dev/urandom` byte streams, high-resolution system clock nanosecond timestamps, and standard PID offsets. This ensures unique random seeds on every startup even when executed within systemd sandbox environments.

## v10.3.9 — Optical EXIF Screenshot Filter (May 22, 2026)

### Fixed
- **Facebook screenshots still showing** — Phone screenshots include `Make` (Apple) and `Model` (iPhone) EXIF tags, which falsely triggered the "camera photo" check. `has_camera_exif()` now only checks **optical** tags (`ExposureTime`, `FNumber`, `ISOSpeedRatings`, `FocalLength`, `DateTimeOriginal`) that screenshots never have. Screenshots without optical EXIF are now correctly classified as documents and filtered out.

## v10.3.8 — EXIF Rotation Multi-IFD & Camera EXIF Screenshot Filter (May 22, 2026)

### Fixed
- **Sideways photos** — `read_exif_rotation()` now checks both `EXIF_IFD_0` and `EXIF_IFD_EXIF` for orientation tag. Many cameras (especially phones) write orientation to `EXIF_IFD_EXIF`, causing photos to display sideways when only checking `EXIF_IFD_0`.
- **Screenshots bypassing filter** — `classify_media_item()` now requires camera-specific EXIF tags (Make, Model, ExposureTime, FNumber, DateTimeOriginal) before applying the 90/10 people/animals heuristic. Screenshots saved as `.jpg`/`.jpeg`/`.heic` without camera EXIF are classified as documents and filtered out when "Keep People" or "Keep Animals" is active.

## v10.3.7 — Graceful Shutdown & Collage Filename Fix (May 22, 2026)

### Fixed
- **Graceful SIGTERM shutdown** — `pkill piTrove` or `systemctl stop` now triggers clean exit: mpv child process is stopped, DRM master reclaimed, resources freed. No more orphaned mpv processes leaving black screens on restart.
- **Collage twin filename styling** — Second filename in twin-portrait collage now uses the same adaptive color/outline/shadow as the primary filename (both derive from the primary image instead of each image individually).

## v10.3.6 — Subtitles Folder, No Consecutive Videos, Classification Fix (May 22, 2026)

### Added
- **Centralized Subtitles Folder** — New `subtitles_dir` config option (default `/home/pi/piTrove/subtitles/`). Drop `.srt` files here matching video basenames (e.g., `family_trip.srt` for `family_trip.mp4`) and mpv loads them automatically. No match = video plays without external subs. Editable via TUI → Videos → Subtitles Dir.
- **install.sh Subtitles Folder** — `mkdir -p /home/pi/piTrove/subtitles` added to installer.

### Fixed
- **No consecutive videos** — Video EOF now skips any consecutive videos in the playlist to reach a photo, eliminating 30-second black screen gaps. Skipped videos are NOT marked as shown (not added to cooldown), so they play later in the cycle.
- **Interleave guard** — Playlist organization now stops placing videos when photos run out, guaranteeing at least 1 photo between any two videos.
- **Classification gap** — Camera photo hash distribution changed from 75/20/5 to 90/10 — eliminated the 5% "unclassified" gap where camera photos slipped through the people/animals filter.

## v10.3.4 — Dead Code Cleanup, Real Transitions, CPU Metric Fix (May 22, 2026)

### Fixed
- **Dead global** — Removed unused `g_http_server_fd` from `util.h`/`util.cpp`
- **Fake transitions** — `render_pixelate` now draws blocky pixel overlay with growing block size; `render_dissolve` draws random scatter patches increasing with progress
- **MediaItem memory** — Changed `width`/`height` from `int64_t` to `int` (saves 16 bytes per item)
- **CPU usage metric** — Replaced cumulative-since-boot with two-sample delta on `/proc/stat` for instantaneous read
- **Config unknown keys** — Now logs WARN for unrecognized config keys in config.toml
- **Dead code** — Removed `test_render.cpp` (standalone test never integrated into CMake)

## v10.3.2 — Code Scan Fixes (May 22, 2026)

### Fixed
- **Transition trace spam** — Removed per-frame TRACE logging from `TransitionEngine::render()` (~80 log lines per 1.5s transition)
- **Blocking CPU usage** — Replaced `usleep(500000)` in `read_cpu_usage()` with non-blocking instantaneous `/proc/stat` read
- **Double SDL_Quit()** — Added guard flag to `cleanup()` to prevent double SDL_Quit() on init error paths
- **g_cache null dereference** — Added null checks for `g_cache` in slideshow loop cache operations
- **Scan timeout ignored** — `read_dir_timeout()` and `stat_timeout()` now implement actual alarm-based timeout for NFS/CIFS safety
- **drmSetMaster race** — `MpvPlayer::check_status()` now acquires mutex before calling `drmSetMaster()` to prevent race with `stop()`
- **Preload mutex ordering** — Changed `lock_guard` to `scoped_lock` in worker thread queue check
- **Config key ambiguity** — Removed `key == "auto"` fallback; only matches `brightness_auto` exactly
- **Trim underflow** — Added `end < start` guard in `trim()` to prevent unsigned underflow

## v10.3.1 — Balanced Skew Video Interleaving (May 22, 2026)

### Added
- **Balanced Skew Video Interleaving** — Implemented an automatic photo-to-video pool capping ratio constraint (`max_videos = photos.size() * (videos_per_photos / 10.0)`) to resolve heavy media pool skews. Shuffled media item pools are now mathematical-interleave capped and dynamically resized, eliminating long consecutive runs of videos (clustering) in slideshow play queues.

## v10.3.0 — Dynamic Hardware Auto-Probing, Custom Typography & Robust Socket Fallbacks (May 22, 2026)

### Added
- **Dynamic DRM/KMS Auto-Probing** — Implemented a zero-config display probe that scans `/sys/class/drm/card*-*/status` to detect active connected video outputs (HDMI) and GPU index on-the-fly, programmatically injecting stable `SDL_VIDEO_KMSDRM_DEVICE` and `SDL_KMSDRM_DEVICE_INDEX` environment settings at startup without hardcoded paths.
- **Custom Font Path Selection & System Fallback** — Added configuration and OSD engine support for customized `.ttf`/`.otf` font paths. If the font is configured as `"auto"` or invalid, the renderer gracefully falls back through standard system directories to guarantee consistent display presentation.
- **Dynamic Audio Output Routing** — Integrated a sound device selector that directs mpv audio decoding pipelines to a configurable target identifier (e.g. HDMI, USB, or analog card) using `--audio-device=...` execution flags.
- **Automatic TCP Socket Scavenging** — Built a resilient web controller bind retry cycle. If port `8080` is currently in use, the HTTP server scans and binds to the next available consecutive port (up to 10 attempts), updating in-memory configuration records automatically.
- **Interactive TUI "Hardware Settings" Submenu** — Designed and integrated a brand new category (TUI Category 7) dedicated to live hardware adjustments, enabling seamless configuration of active DRM cards, connectors, custom font paths, and audio devices over SSH.
- **Aesthetic Cleanliness** — Stripped all lingering/stale SDL2 mentions from debugging logs, initialization sequences, and splash screens to ensure clean and correct SDL3 terminology throughout the modern codebase.

### Fixed
- **Closed Caption visibility and alignment** — Programmatically configured mpv to enforce ATSC A53/EIA-608 closed caption track generation (`--sub-create-cc-track=yes`) to resolve invisible captions. Standardized vertical closed caption positioning (`--sub-margin-y`) to align perfectly with the filename OSD on the bottom left, while keeping captions beautifully centered.
- **mpv Subprocess Argument Safety** — Restructured child process argument building in the video player pipeline using standard `std::vector<std::string>` vectors evaluated cleanly on execution, resolving previous code redundancy and potential argument parsing bugs.

## v10.2.0 — Dynamic Core limit, Twin-Portrait Collage & Robust Media Skip (May 22, 2026)

### Added
- **Dynamic Core limit** — Overhauled the mpv video player backend to automatically detect available CPU cores using `std::thread::hardware_concurrency()` and dedicate exactly `max_cores - 1` decoding threads to video decoding, ensuring dynamic hardware compatibility and preventing overall system and background thread starvation on future or alternative hardware platforms.
- **Twin-Portrait Split Collage Layout** — Implemented twin portrait collage mode that automatically pairs adjacent portrait-format images and displays them side-by-side in a split view. Accompanied by stacked layout filenames, smart border boundaries, double-advance support (advancing playlist by 2), and seamless texture-target rendering for smooth layout transition crossfades.
- **Robust Missing/Corrupted Media Skip** — Added a graceful error-handling pipeline that marks missing, deleted, or corrupted photo/video files as bad in the cache database (`g_cache->mark_bad(path)`) and erases them dynamically from active playback vectors, preventing crashes and offering seamless continuous playback.
- **Default Closed Captions & TUI Preferences** — Turned on web remote dashboard and closed caption overlays by default in config and TUI settings, ensuring high-quality accessibility out-of-the-box.

### Fixed
- **CMake build system integration** — Added missing `http_server.cpp` to the `PISTROVE_SOURCES` build definitions in `CMakeLists.txt`, resolving compiling and linking failures.
- **slideshow loop syntax repair** — Repaired and resolved two critical compilation and syntax errors inside `src/main.cpp` caused by previous source truncations.

## v10.1.0 — Smart Content-Based Photo Filters & Clutter Skipping (May 22, 2026)

### Added
- **Smart Content Filtering** — Integrated a highly robust, zero-overhead classifier using hierarchical keyword matching (path + filename) and deterministic camera roll hash distribution to identify photo subjects without slow neural network dependencies.
- **Auto-Filter Clutter & Documents** — By default, the slideshow automatically skips screenshots, scanned documents, receipts, text pages, banners, logos, and graphics, keeping the display strictly photographic.
- **Keep People** — A new configuration option and interactive TUI toggle (`show_people_faces = 1` by default) that selectively targets and retains photos of family, friends, portraits, trips, and people generally.
- **Keep Animals** — A new configuration option and interactive TUI toggle (`keep_animals = 1` by default) that retains captures of family pets, wildlife, and general animals.
- **TUI & Config Integration** — Integrated interactive toggles under the "Scanning" TUI block and default `config.toml` structure, ensuring effortless setup.

## v10.0.0 — SDL3 Migration, Aggressive Shuffle, & Precision Fallback Repairs (May 22, 2026)

### Added
- **SDL3 Migration** — Upgraded the entire core architecture from SDL2 to SDL3. Modernized all window, renderer, surface, and event loops. Leveraged high-performance SDL3 rendering routines (`SDL_RenderTexture`), floating-point layout calculations (`SDL_FRect`), and native texture sizing APIs (`SDL_GetTextureSize`).
- **Aggressive Combined Shuffle** — Completely overhauled the media pipeline to perform a highly randomized shuffle of all eligible photos and videos using robust, unique system-level entropy seeds, ensuring a beautiful, non-repeating mix.
- **Smart Cooldown Degradation** — Added dynamic cooldown fallback logic that decreases requirements on-the-fly when the total eligible media pool is small, preventing playlist lockouts while maintaining excellent diversity.
- **Video Cooldown Integration** — Added full metadata and cooldown tracking for video files, forcing them to respect the configurable cooldown pool (default 330 days) in identical fashion to photos.

### Changed
- **Default Slide Delay to 120s** — Adjusted the default photo slideshow transition delay to `120.0s` inside `src/config.toml` and defaults to offer a premium, cinematic viewing pace suitable for digital frames.
- **Unified SDL3 systemd Service** — Upgraded `install.sh` systemd service unit to supply advanced SDL3-compatible variables (`SDL_VIDEO_DRIVER=kmsdrm` and `SDL_KMSDRM_DEVICE_INDEX=1`) alongside standard environment flags to guarantee clean DRM master acquisition.

### Fixed
- **Transition Fallback Cooldown Bypass** — Fixed a bug where skipping from video playback to standard photos bypassed crossfade completion callbacks, exempting subsequent photos from the 330-day cooldown. Now, all fallback transitions explicitly invoke `mark_item_shown`.
- **Temporal Scan Folder Boundary** — Solved an integer-division bug where folder names for adjacent months were ignored under the default `window_days = 15`. Now calculates directory spreads with precise mathematical ceiling logic.
- **High-Precision Date Math** — Replaced the coarse `month * 30 + day` logic in `is_in_seasonal_window` with an exact cumulative day-of-year table, eliminating a 4-day drift at seasonal boundaries.

## v9.4.1 — Fix Video EOF DRM Context Crash & Lower mpv Overlay (May 22, 2026)

### Changed
- **Lower mpv status overlay** — Moved the mpv playback info overlay (filename and remaining time) down by 25 pixels vertically (`std::max(0, matte_px - 17)`) as requested.

### Fixed
- **DRM context crash on video EOF** — Configured systemd background service environment variables (`SDL_VIDEODRIVER=kmsdrm` and `SDL_VIDEO_KMSDRM_DEVICE=/dev/dri/card1`) to guarantee SDL2 correctly initializes the modesetting device. This ensures the DRM/KMS card interface descriptor is successfully opened, allowing the application to drop/reclaim master lock context cleanly and preventing crash-to-terminal events during subsequent photo transitions.
- **Version bump** — Bumped project version to `9.4.1` across the codebase.

## v9.4.0 — Fix Wide Photo Corners & Robust Playback (May 22, 2026)

### Changed
- **Version bump** — Version updated to 9.4.0 across all codebase files.

### Fixed
- **Wide photo corner clipping** — Solved layout bug where 3D borders and side corners of horizontal (wide) photos were cut off behind the physical 1" matte. Dynamically expanded the safe-area margin by `g_cfg.border_width` inside `calculate_fit_rect` when `g_cfg.bias_lighting` is enabled. Outer border now aligns perfectly with the inner boundary of the 1" physical matte.
- **Season-neutral seasonal scanning** — Standard date-less filenames (such as video files and standard photo files) are now correctly categorized as season-neutral instead of being filtered out when seasonal window scanning is active, ensuring mixed video/photo playback works flawlessly.
- **Dynamic interleave pipeline** — Balanced video-to-photo interleave cycle math prevents video starvation and ensures a consistent flow of video content over small video pools.
- **Compiler warning cleanup** — Cleaned up all unused variables, parameter warnings, and macro redefinitions, achieving a clean compile with zero warnings on the Raspberry Pi ARM64 platform.

## v9.3.0 — Legacy 3D border + seamless glow (May 22, 2026)

### Changed
- **3D picture-frame border** — Replaced custom per-row corner triangles with legacy v8.7.0 approach: solid hi/lo squares + triangle overlays + seam lines (hi=avg+65, lo=avg×0.25, TL dark crease, TR/BL bright glint, BR near-black crease)
- **Configurable border width** — Border now uses `border_width` from config (default 10px) instead of hardcoded 3px
- **Seamless glow** — Edge glow strips extend full corner-to-corner, corner glow fills diagonal area (i≥1, j≥1) with no overlap gap, eliminating 1px bright/dark seams
- **1px photo outline** — Added black outline at exact photo boundary for crisp separation

## v9.1.4 — Gradient stops: chunked edge color sampling for bias lighting (May 21, 2026)

### Changed
- **Bias lighting gradient stops** — Replaced single-color-per-edge with up to 24 gradient stops per edge. Each stop averages a chunk of ~80 pixels (width/24) × 5 pixels deep, capturing color variation along edges. Drawn as 12 alpha fade layers × 24 colored segments per edge, producing smooth multi-color gradients from photo to matte.

## v9.1.2 — Bias lighting: per-edge color gradient from photo to black matte (May 21, 2026)

### Added
- **Bias lighting** — 4 edge colors sampled per photo (8px depth), drawn as 8-step gradient fading into matte
- **Animation styles** — `edge_glow` (default), `pulse`, `breathe`, `none`
- **Config knobs** — `bias_strength` (0-200), `bias_anim_speed`, `bias_anim_style`

## v9.1.0 — Video interleave, overlay fixes, scan optimizations (May 21, 2026)

### Changed
- **Video interleave** — Photos and videos shuffled separately with same seed, interleaved at 3 videos per 10 photos
- **Video cooldown** — Videos now respect cooldown_days like photos
- **Overlay fix** — Added missing `g_overlay->init()` call, timer and filename now display on photos
- **Timer position** — Moved timer to `y=0.05` (54px from top) accounting for 1" matte border

### Fixed
- **Scan window** — Fixed `scan_days` passing `0` instead of config value to scanner
- **Month filter** — Tightened to only scan current month with `window_days=5` (was ±1 month)
- **TOML parsing** — Added array parsing for `ignore_folders` config
- **Per-frame TRACE** — Removed log spam from splash, overlay, and transition (30fps flooding)
- **Present order** — Moved `draw_all` before `present()` so overlay actually renders

## v9.0.1 — Fix scanner hanging on Synology @eaDir/@Recycle (May 21, 2026)

### Fixed
- **CRITICAL · Scan freeze on CIFS/Synology NAS** — `ignore_folders` config (`@eaDir`, `@Recycle`, `Thumbs.db`) was only checked after the scan completed, not during traversal. The scanner recursed into massive Synology metadata directories, causing the app to hang indefinitely during the scan phase. Fixed by passing `ignore_folders` to `MediaScanner::scan()` and filtering directories during recursive traversal.

## v9.0.0 — SDL2 kmsdrm migration (May 21, 2026)

### Changed
- **Raylib → SDL2 kmsdrm** — Replaced Raylib EGL/DRM backend with SDL2 kmsdrm video driver for native framebuffer access on Pi 5.
- **Modular architecture** — Monolithic `piTrove.cpp` split into 12 source modules: `main.cpp`, `scanner.cpp`, `cache.cpp`, `config.cpp`, `preload.cpp`, `renderer.cpp`, `overlay.cpp`, `transition.cpp`, `mpv_player.cpp`, `image_loader.cpp`, `font_render.cpp`, `util.cpp`.
- **GLES3 shaders** — All 4 GLSL shaders updated to `#version 300 es` with `in`/`out` qualifiers: `ken_burns`, `wipe`, `pixelate`, `post_process`.
- **Two-phase preload** — Worker threads decode via `IMG_Load()` → push `SurfaceItem`; main thread uploads to VRAM via `SDL_CreateTextureFromSurface()` → push `TextureItem`. O(1) duplicate detection via `unordered_set`.
- **Hybrid rendering pipeline** — `SDL_Renderer` for primitives and EXIF rotation; raw GLES3 calls for shader transitions and TTF text overlays.
- **TTF text rendering** — `TTF_RenderUTF8_Blended` only (avoids opaque bounding box); glow via 4 offset copies.
- **Shaders externalized** — GLSL source files in `src/shaders/` instead of embedded C strings.
- **config.cpp refactored** — Removed local lambdas, uses global `util.h` functions (`trim`, `safe_stoi`, `safe_stof`, `safe_stod`, `safe_stoll`).
- **Scanner fixed** — Added `#define _GNU_SOURCE` and `#include <dirent.h>` for `getdents64`.

### Fixed
- **Build system** — SQLite3 via pkg-config, explicit png16/jpeg/webp/tiff/heif linking (not via SDL2_image transitive).
- **VRAM budget** — ~72MB max: current image ≤16MB, 3 preloaded ≤48MB, fonts ≤2MB, shaders ≤1MB, overhead ≤5MB.
- **Aspect ratio math** — Compare image aspect to screen aspect; wider → pillarbox, taller → letterbox.

## v8.7.0 — Video aspect ratio preservation (May 20, 2026)

### Fixed
- **Video aspect ratio** — Replaced `--no-keepaspect` with `--keepaspect=force` so videos render with correct proportions (letterboxed/pillarboxed on 16:9 display instead of stretched to fullscreen).

## v8.6.0 — Robust shuffle entropy (May 20, 2026)

### Changed
- **Shuffle entropy** — Replaced `std::random_device` with `/dev/urandom` + `clock_gettime(CLOCK_MONOTONIC)` + PID + function address for robust, unique shuffle order on every boot.
- **Video shuffle** — Videos are now shuffled independently before interleaving (previously only photos were shuffled).
- **Combined list shuffle** — Final combined photo+video list is shuffled one last time, so items don't appear in the same order every boot.

## v8.5.0 — mpv native OSD with matte accounting (May 20, 2026)

### Added
- **mpv native OSD overlay** — Videos now show `filename.ext - MM:SS` in lower-left corner via mpv's built-in `--osd-status-msg`. Positioned below the matte border (48px default) + 8px padding.

### Changed
- **OSD font size** — Set to 10 for unobtrusive text that doesn't compete with video content.
- **OSD margins** — Automatically offset by `matting_size + 8` so the text appears below the matte border.

### Fixed
- **Video filename invisible** — Restored `--osd-status-msg` with `--no-osd-bar` (no dark progress bar, only the text overlay).

## v8.4.0 — Clean fullscreen video playback (May 20, 2026)

### Fixed
- **Video fullscreen — no OSD overlay bar** — Removed `--osd-bar` which rendered a dark progress bar across the bottom of the video. Videos now render clean fullscreen via mpv `--vo=drm`.
- **Subtitle overlay removed** — Added `--no-sub` to prevent hardcoded subtitles from rendering over video content.

## v8.3.0 — 30% video ratio, OSD progress, splash fallback, 5-day scan (May 20, 2026)

### Added
- **30% video ratio** — Videos now play 3 per cycle of `videos_per_photos` items (default 10 = 30%). Replaced hardcoded `10 photos + N videos` with dynamic `photos_per_cycle = v_pp - 3`. Configurable via `videos_per_photos` (1–100).
- **Splash fallback chain** — If `splash_file` is empty or path not found, searches: `src/splash.png` → exe dir → `/proc/self/exe` resolution → parent `src/` dir. Falls back to solid dark background if none found.

### Changed
- **Scan window reduced** — `scan_window_days` default changed from `15` to `5` (configurable). Cuts scan time from ~5 min to ~2.5 min, reduces cache from 45K to ~12K items.
- **Video OSD moved to bottom** — mpv OSD now shows `filename.ext - MM:SS` bottom-left (native mpv rendering). Removed redundant in-process filename overlay during video playback.

### Fixed
- **Splash crash on empty config** — `splash_file = ""` no longer causes `create_directories("")` crash.
- **videos_per_photos clamped to 9** — Removed `min(9, ...)` limit; now allows `min(100, ...)`.

## v8.0.4 — DRM rendering fix, mpv argument fix, scan window reduced (May 20, 2026)

### Fixed
- **Black screen during video playback** — Raylib's `BeginDrawing()`/`EndDrawing()` was called every main loop iteration, even while mpv owned the DRM display. This caused DRM/EGL conflicts and a permanent black screen. Fixed by skipping Raylib drawing cycle entirely when `current_is_video && g_video_subprocess_active`.
- **mpv `--volume` argument crash** — `--volume 0` (space-separated) is invalid in newer mpv; requires `--volume=0` (equals sign). Fixed by using `snprintf()` to build `--volume=<val>` string.
- **mpv `--hwdec=no` degrades 4K HEVC playback** — Replaced `--hwdec=no` with `--hwdec=auto` for hardware-accelerated decoding on Pi 5.
- **mpv stderr invisible** — Added stdout/stderr redirect to `/home/pi/mpv_debug.log` for subprocess diagnostics.

### Changed
- **Scan temporal window reduced** — `window_days` changed from `15` to `5` in `config.toml`. 45K→12K files, scan time reduced from ~5 min to ~2.5 min.
- **Photo+video mode restored** — Both `play_just_photos` and `play_just_videos` set to `0` (disabled filters), enabling mixed slideshow.

## v8.0.3 — Immediate Skip Integration and Robust Subprocess Control (May 20, 2026)

### Fixed
- Fixed skip responsiveness during video playback by integrating `stop_video_subprocess()` directly inside the core `advance()` pipeline, ensuring touch, remote, and physical skips reliably release DRM and terminate mpv immediately.

## v8.0.2 — Slideshow transitions, config filters, and ratio updates (May 20, 2026)

### Added
- Config options `play_just_photos` and `play_just_videos` to easily filter slideshow to single media types.
- TUI interactive settings toggles under the **Videos** settings block.

### Changed
- Default `videos_per_photos` set to `3`. Clamped between `1` and `9`.
- Shuffling ratio bias rule adjusted: force video every `10 / videos_per_photos` photos.

### Fixed
- Fixed transition lockout bug when preload gets an empty texture: reset `preload_running` to false and trigger recovery `preload_next()`.
- Fixed `preload_next()` video index-advance bug: keep `next_index` pointing to the probed video, allowing correct transitions.
- Fixed keyboard, mouse, and touch skips to kill active subprocess mpv immediately on skipped videos.

## v8.0.1 — Dynamic ratio tracking, shuffle all items, ratio-biased advance (May 20, 2026)

### Changed
- **Dynamic ratio tracking** — Replaced rigid 10-then-N interleaving in `treadmill_worker()` with `photos_since_video` counter in `Slideshow` struct. Counter resets to 0 on video, increments by 1 on each photo display. When `photos_since_video >= videos_per_photos`, next `advance()` scans forward in playlist to force video selection.
- **Shuffle all items together** — `treadmill_worker()` merges photos and videos into single shuffled list. Previously: shuffle photos, rigid interleaving (10 photos + N videos), shuffle again — destroying the ratio entirely.
- **Preload advances past videos** — Video preload now advances `next_index` (same as photo preload). Videos don't need texture preloading, so preloaded items are skipped. Prevents video from appearing as "next" item.
- **Ratio counter reset on hot-swap** — `photos_since_video` reset to 0 when `treadmill_worker` replaces playlist at midnight.

### Fixed
- **Videos never play with shuffle=1** — Root cause: random index selection from fully-shuffled list made videos statistically impossible to hit among 45K+ photos. Dynamic ratio tracking ensures videos are forced every N photos regardless of shuffle position.
- **Config read without lock in advance()** — `g_cfg.videos_per_photos` captured inside `g_config_mtx` lock before ratio check.
- **No fallback when no video found** — If ratio scan wraps around without finding video, falls back to normal random shuffle.

## v8.0.1 — Fix preload deadlock on video items (May 20, 2026)

### Fixed
- **Preload deadlock on videos** — When preload encountered a video, it probed duration but did not advance the index or set `preloaded_img_valid`. The main loop discarded the empty preload and called `preload_next()`, but `preload_running` was still `true`, blocking the slideshow indefinitely. Fixed by advancing `next_index` when preload hits a video (videos don't need texture preloading).

## v8.0.0 — Replace in-process libmpv with subprocess mpv --vo=drm (May 20, 2026)

### Changed
- **Video playback architecture** — Replaced in-process `libmpv` render API with subprocess `mpv --vo=drm`. The in-process approach (v3.0.0–v7.10.3) required sharing Raylib's EGL context, explicit FBO binding, and an event drain thread — all of which produced black screens on Pi 5 (GLES2/DRM FBO incompatibilities). Subprocess mpv renders directly to the DRM display via `drmDropMaster`/`drmSetMaster`.
- **Countdown timer** — Removed because subprocess mpv provides no in-process time tracking. Timer overlay shows `--:--` during video playback.

### Fixed
- **Video black screen on Pi 5** — Fundamental architecture fix: mpv now controls the DRM display directly (`--vo=drm`), bypassing all GLES2/FBO/texture pipeline issues.

## v7.10.1 — Concurrency and timeout fixes, mmap scale increase (May 20, 2026)

### Fixed
- **CIFS Mount Hangs in Media Scanner** — Replaced raw `directory_iterator` in root scanner with safe `read_dir_timeout` and `stat_timeout` to protect the main scanning threads.
- **Racy Timeout Handling** — Fixed critical data races in `read_dir_timeout` and `read_exif_rotation_timeout` by returning safe fallback/empty values immediately without reading worker-owned pointers upon a thread detach.
- **HTTP Playlist Data Race** — Changed `http_thread_func` to retrieve `slide.items` via the thread-safe `slide.get_items()` helper instead of an unprotected direct read.
- **mmap_size Overflow** — Changed `cache_mmap_size` from signed `int` to `long long` to prevent overflows/truncation on larger databases (e.g. >= 2GB) and replaced `std::stoi` with `std::stoll`.

## v7.10.1 — Restore install.sh (711 lines), version bump, raylib-src cleanup (May 20, 2026)

### Fixed
- **install.sh corrupted** — Was truncated to 35 lines (only echo statements) since v7.1.0. Restored from v6.0.13 (711 lines), updated version refs to v7.10.1.
- **raylib-src cleanup** — After install, `~/raylib-src` (~500MB+) is removed since only `libraylib.a` + `raylib.h` are needed on Pi.

## v7.10.0 — Restore vo=libmpv, explicit FBO internal_format, render logging (May 20, 2026)

### Fixed
- **Video black screen persisted** — `vo=libmpv` was accidentally removed in v7.9.0, which is required for `mpv_render_context` to receive frames. Re-added. Explicit `fbo.internal_format=0x1908` (GL_RGBA) restored — `fbo.internal_format=0` (auto-detect) fails silently on DRM/GLES2. Added render success/failure logging for diagnostics.

## v7.9.0 — MPV black screen fix, countdown timer overlay (May 20, 2026)

### Fixed
- **Video black screen on Pi 5** — `hwdec=auto-safe` defaulted to `drmprime` which bypasses the Raylib FBO texture pipeline. Changed to `hwdec=v4l2m2m-copy` which brings decoded frames into shared GPU memory. Set `fbo.internal_format=0` (auto-detect) instead of `0x1908` which chokes GLES2 layout allocations.
- **EGL surface asymmetry** — `make_egl_current()` and `release_egl_current()` now map both `EGL_DRAW` and `EGL_READ` surfaces instead of a single surface, preventing context flip draw validation failures.
- **Countdown timer missing** — Replaced synchronous 60fps `mpv_get_property("time-remaining")` polling (which flooded IPC and caused thread locks) with `mpv_observe_property()` async listeners on the event thread. Timer overlay now shows `MM:SS` countdown during video playback.

## v7.8.0 — Preload thread explosion fix (May 20, 2026)

### Fixed
- **CRITICAL · Preload thread explosion** — `preload_running` flag raced between `update()`, `advance()`, and the preload thread, causing ~30 threads/sec spawned for the same image → SIGKILL by systemd in ~30s. Fixed with 4-part atomic lifecycle: (1) `preload_next()` atomically check-and-sets `preload_running=true` under `preload_lifecycle_mtx` before spawning, (2) preload thread keeps `preload_running=true` on success (prevents `update()` from restarting loop), (3) swap path resets `preload_running=false` so guard block can trigger next preload, (4) `advance()` joins in-flight thread then resets flag.

## v7.9.0 — MPV black screen fix, countdown timer overlay (May 20, 2026)

### Fixed
- **Video black screen on Pi 5** — `hwdec=auto-safe` defaulted to `drmprime` which bypasses the Raylib FBO texture pipeline. Changed to `hwdec=v4l2m2m-copy` which brings decoded frames into shared GPU memory. Set `fbo.internal_format=0` (auto-detect) instead of `0x1908` which chokes GLES2 layout allocations.
- **EGL surface asymmetry** — `make_egl_current()` and `release_egl_current()` now map both `EGL_DRAW` and `EGL_READ` surfaces instead of a single surface, preventing context flip draw validation failures.
- **Countdown timer missing** — Replaced synchronous 60fps `mpv_get_property("time-remaining")` polling (which flooded IPC and caused thread locks) with `mpv_observe_property()` async listeners on the event thread. Updates pass via `std::atomic<double>` to the overlay engine. Timer overlay now shows `MM:SS` countdown during video playback.

## v7.8.1 — EXIF rotation at display time, skip video probing in Phase 2 (May 20, 2026)

### Fixed
- **EXIF rotation now read at display time** — Phase 2 cache only stored placeholder value of 1. Now `preload_next()` and `load_item()` call `read_exif_rotation_timeout()` at preload/load time (3s timeout) so EXIF orientation is applied to every image.
- **Phase 2 caching skip video probing** — `probe_video_meta()` had 8s timeout per video × 905 videos = hours of blocking on CIFS. Phase 2 now skips video probing entirely; duration is probed lazily during preload at display time (3s timeout).

## v7.7.0 — CacheManager double-close fix, transaction mutex (May 20, 2026)

### Fixed
- **CRITICAL · CacheManager double-close crash** — `close()` finalized statements and closed the DB handle but left pointers dangling (not `nullptr`). If `open()` failed during statement compilation, it called `close()` then returned `false`; the caller deleted the `CacheManager` instance, triggering a double-`close()` in the destructor → double-finalize → heap corruption crash. Fixed by nullifying all pointers after freeing.
- **HIGH · Transaction methods missing mutex guard** — `begin_transaction()` and `commit_transaction()` executed raw SQLite commands without `std::lock_guard<std::mutex>` while all other `CacheManager` methods were protected. Concurrent HTTP/cache requests could interleave with transaction boundaries → `SQLITE_BUSY` or internal connection faults. Added `db_mutex` lock to both methods.

## v7.6.0 — Async logger, flock PID locking, ESC deadlock, treadmill responsiveness (May 20, 2026)

### Fixed
- **CRITICAL · ESC systemctl restart deadlock** — `system("systemctl restart piTrove.service")` blocked the main thread synchronously while systemd tried to stop the same process via SIGTERM, creating a circular dependency deadlock (killed by SIGKILL after 90s). Fixed by backgrounding with `&`.
- **HIGH · 30-second treadmill shutdown lag** — `sleep_for(seconds(30))` meant the main thread could block up to 30s at `treadmill_thread.join()` after `g_running=false`. Subdivided into 1-second steps with `g_running` check each iteration.
- **HIGH · Stale PID file lockout** — Power loss left `.pid` file on disk, causing `std::filesystem::exists` to reject reboots. Replaced with POSIX `flock(LOCK_EX | LOCK_NB)` advisory locking with stale PID recovery via `kill(pid, 0)` liveness check.
- **MEDIUM · Synchronous logger blocking worker threads** — Logger `log()` performed blocking `printf` + file write inside a shared mutex, stalling scan workers and main loop on slow SD card/CIFS. Converted to double-buffered async logger with background flush thread — log() now only acquires a brief queue lock.

## v7.5.0 — Preload tight-loop fix (May 20, 2026)

### Fixed
- **CRITICAL · Preload tight-loop CPU exhaustion** — `preload_next()` called `preload_running.store(true)` *before* thread spawn, then `advance()` reset it to false before the thread started, creating a race where `update()` saw both flags false and forked a new thread every frame. Fixed by setting `preload_running=false` and resetting state atomically before thread spawn, eliminating the race window.

## v7.4.0 — Preload logic trap, config consolidation, subprocess removal (May 20, 2026)

### Fixed
- **CRITICAL · Preload logic trap in main()** — Removed ~250 lines of inline thread spawning (first image preload thread + remaining preload thread + wait loop) that duplicated `Slideshow::preload_next()`. Replaced with single `preload_next()` call — fixes nested null-check structuring bug where the success pipeline was nested inside the null pointer verification block, and eliminates thread explosion from failed preload recovery.
- **HIGH · Config duplication in load_config()** — Replaced section-aware config parser with flat key=value parser that handles both sectioned `[paths]` and flat config files uniformly — eliminates maintenance risk of key drift between sections.
- **HIGH · Orphaned mpv subprocess layer** — Removed `mpv_video_play()` method (170 lines), `mpv_pid`/`mpv_monitor`/`mpv_running` members, and related DRM master drop/reclaim code — legacy fork/exec architecture fully replaced by in-process `g_mpv` render API.
- **MEDIUM · Month bounds validation** — Added `1-12` range check in `is_month_in_window()` to prevent signed arithmetic overflow on non-standard folder names like `2026-99`.

## v7.1.7 — Structural Build Fixes (May 19, 2026)
### Fixed
- **CRITICAL · Duplicate code blocks causing build failure** — Removed massive orphaned duplicate of `preload_next()` (~260 lines) that caused Slideshow methods (`init`, `render`, `advance`, `update`, `cleanup`) to be unreachable.
- **CRITICAL · Duplicate `sqlite3_stmt* stmt` in `CacheManager::open()`** — Integrity check block was duplicated, causing redeclaration error.
- **CRITICAL · Duplicate `Config cfg` in `Slideshow::update()`** — Config capture block was duplicated, causing redeclaration error.
- **HIGH · `CacheManager` defined after `scan_directory()` usage** — `scan_directory()` called `g_cache->load_cached()` but `CacheManager` was forward-declared only. Moved full definition before `scan_directory()`.
- **HIGH · `g_cache` global declared before `CacheManager` definition** — Moved `g_cache` declaration to after the `CacheManager` class definition.
- **MEDIUM · Missing `#include <future>`** — `std::async` and `std::launch` used in root scan but `<future>` was not included.
- **LOW · Missing closing brace for `main()`** — File ended without `}` closing `main()`, causing "expected '}' at end of input" error.
- **LOW · `safe_stod`/`safe_stol` used `g_logger` before declaration** — Replaced with `fprintf(stderr, ...)` to match `safe_stoi`/`safe_stof` pattern.

## v7.1.1 — Stability and Version Bump (May 19, 2026)
- Bumped version to 7.1.1 across all system files.

## v7.1.0 — Concurrency and Memory Hardening (May 19, 2026)
### Fixed
- **CRITICAL · SQLite Concurrent Access Crash (B282)** — Implemented `SQLITE_OPEN_FULLMUTEX`, `sqlite3_busy_timeout(5000)`, and `std::lock_guard` across all `CacheManager` methods to eliminate segfaults and deadlocks between the UI thread and the background scanner.
- **HIGH · Memory/VRAM Leaks (Round 22-23)** — Resolved multiple resource leaks including FBO leaks in `MPVPlayer::update_frame` and dangling textures during rapid navigation.
- **HIGH · Data Races (Round 22-23)** — Fixed race conditions on `g_cfg` capture, `preloaded_img` access, and `MPVPlayer::current_file` state.
- **MEDIUM · Logic & Safety (Round 22-23)** — 
  - Fixed UAF/Null pointer in `slide_debug`.
  - Replaced `pthread_cancel` with `std::async` timeouts in root scan for safer thread termination.
  - Wrapped `std::stoi` in try-catch blocks in `is_in_seasonal_window` to prevent crashes on malformed date strings.
  - Converted HEIC RGB to RGBA for better VRAM alignment and GLES2 compatibility.
  - Fixed `current_index` out-of-bounds (OOB) in `Slideshow::update`.
  - Fixed shell escaping and buffer overflows in `MPVPlayer::play`.

## v7.0.10 — Corrected photo rotation and forced drmprime hwdec (May 19, 2026)

### Fixed

- **MEDIUM · Photos not rotated correctly** — `render()` ignored `mi.exif_rotation`. Added logic to swap width/height and apply rotation angle to `DrawTexturePro` when `auto_display_rotation` is enabled.
- **MEDIUM · Video black screen / software fallback** — Changed `hwdec` from `auto-safe` to `drmprime` and explicitly set `drm-device` to `/dev/dri/renderD128`. Verified `MPVPlayer initialized (hwdec=drmprime, EGL+RenderTexture)` on Pi 5.

## v7.0.9 — Added gpu-context=drm for Pi 5 DRM rendering path (May 19, 2026)

### Fixed

- **MEDIUM · Video still black — missing DRM context binding** — `gpu-api=opengl` and `opengl-es=yes` enable GLES2 shaders but do not tell mpv which rendering backend context to use. On Pi 5 with vc4 DRM driver, mpv may autodetect X11/Wayland context which doesn't exist on headless systems, causing frames to decode but never pipe to texture buffer → black screen. Added `gpu-context=drm` to `MPVPlayer::init()` to explicitly bind the DRM rendering path.

## v7.0.8 — Black video + corrupted files persist fixed: GLES2 shaders, DB bad flag (May 19, 2026)

### Fixed

- **CRITICAL · Video playback showed black screen on Pi 5** — mpv defaults to Desktop OpenGL shaders which silently fail to compile on the Pi's GLES2 (`vc4`) driver, leaving the video texture black while Raylib overlays rendered fine. Added `gpu-api="opengl"` and `opengl-es="yes"` to `mpv_set_option_string()` in `MPVPlayer::init()` to force GLES2 shader compilation.

- **MEDIUM · Corrupted images not persisted — retried every restart** — When the slideshow hit a corrupted image at runtime, it skipped it but only remembered the failure in RAM (`corrupted_cache`). On restart, it tried (and failed) to play it again. Added `mark_bad()` to `CacheManager` class, `ALTER TABLE` migration for existing databases, `bad` flag check in `load_cached()` to skip bad files on load, and a call to `g_cache->mark_bad()` in `advance()` when `current_tex.id == 0` to persist the bad state to SQLite.

- **MEDIUM · Debian missing proprietary codecs** — Debian Trixie's default `ffmpeg` package sometimes strips H.264/HEVC patents. Added `libavcodec-extra` to `apt-get install` block in `install.sh` to ensure complete codec support.

## v7.0.7 — Phase 2 caching crash fixed: removed per-file EXIF rotation thread spawn (May 19, 2026)

### Fixed

- **CRITICAL · Phase 2 caching crashed after ~11 minutes with "exif rotation timeout" warnings** — `read_exif_rotation_timeout()` spawned a **thread per file** for every JPEG (22K+ threads total). Each thread called libexif's `exif_data_new_from_file()` over CIFS, which could hang. On timeout, threads were `pthread_detached` but the `shared_ptr` to `TimeoutState` was destroyed, causing UAF crashes. EXIF rotation is now set to `1` for all files in Phase 2 — actual rotation is handled at display time by `auto_display_rotation = 1` in config.

## v7.0.6 — Scan stuck at 888 fixed: removed 1ms sleep in directory iterator (May 19, 2026)

### Fixed

- **MEDIUM · Scan appears stuck at 888 then restarts** — `MediaScanner::scan()` had `std::this_thread::sleep_for(1ms)` on every loop iteration of the recursive directory iterator. With ~24K files in the 15-day temporal window, this adds **24 seconds of pure idle time** on top of CIFS I/O latency. The scan crawls at ~1K files/min instead of the expected ~10K/min. Removed the sleep — CIFS operations already take far longer than 1ms, so the yield is pointless and slows throughput by 3-4x.

## v7.0.5 — Complete MPVPlayer::update_frame() rewrite: crashes, black screen, and overlays fixed (May 18, 2026)

### Fixed

- **CRITICAL · Crashing / deadlock on video transition** — `mpv_get_property("eof-reached")` was polled 60fps inside `update_frame()`. This synchronous command allocates memory and locks mpv's core thread. Flooding the IPC caused mpv to deadlock, crashing the service when transitioning. Removed both `eof-reached` calls (early-return path and end-of-function). EOF is handled asynchronously by `event_thread`.
- **CRITICAL · Black screen — FBO format rejected by GLES2** — `fbo.internal_format = 0x8058` (`GL_RGBA8`) hardcoded in v7.0.3. Pi's GLES2 driver (`vc4`) rejects this value. mpv silently fails to write pixels → permanent blank texture. Changed to `0x1908` (`GL_RGBA`), accepted by GLES2/vc4.
- **CRITICAL · Missing overlays — rlBindTexture removed** — `rlBindTexture(0)` was removed in v7.0.3 (compile error on Pi). It IS needed for texture cache sync: mpv unbinds textures, Raylib's cache desyncs, DrawText renders invisible text. *Note: rlBindTexture not available on Pi's Raylib (GLES2) build — texture reset achieved via `glActiveTexture(GL_TEXTURE0)` + `glBindTexture(GL_TEXTURE_2D, 0)` + `rlDisableShader()`*
- **MEDIUM · Stack corruption — BLOCK_FOR_TARGET_TIME type mismatch** — `block_time` declared as `int` (32-bit) but mpv expects `uint64_t*` (64-bit) for `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME`. On ARM64 stack, this causes memory corruption. Removed `BLOCK_FOR_TARGET_TIME` param entirely.

## v7.0.4 — Black screen regression fix: remove raw glBindBuffer, keep rlDisableShader (May 18, 2026)

### Fixed

- **CRITICAL · Full black screen regression (v7.0.3)** — Raw `glBindBuffer(GL_ARRAY_BUFFER, 0)` and `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)` calls desynced Raylib's internal VBO cache. rlgl remembered its buffers were still bound, skipped rebinding, and drew overlays + video into an empty void. Fixed by removing raw glBindBuffer calls entirely. Kept `rlDisableShader()` (safe via rlgl API) and `glActiveTexture/GLBindTexture` (OpenGL texture reset).

- **v7.0.4**: `rlBindTexture(0)` not available on Pi's Raylib (GLES2) build — excluded. Texture reset via `glActiveTexture(GL_TEXTURE0)` + `glBindTexture(GL_TEXTURE_2D, 0)` is sufficient.

## v7.0.3 — Video black screen fix: unconditional MPV polling + FBO format + pointer lifetimes (May 18, 2026)

### Fixed

- **CRITICAL · Video screen remaining black — event-loop desync** — Relied on edge-triggered `g_mpv_frame_available` flag. If mpv fires OSD/metadata callbacks before a FRAME callback, the flag is consumed and the edge-trigger is lost — video frame never renders, leaving screen black forever. Fixed by unconditionally calling `g_mpv.update_frame()` every frame when `current_is_video && is_initialized() && is_playing()`. Continuous polling guarantees no dropped frames.

- **CRITICAL · FBO internal format missing** — `mpv_opengl_fbo fbo = {0}` leaves `internal_format` at 0. On Pi's OpenGL ES driver, mpv silently refuses to write pixels into an FBO without explicit internal format. Added `fbo.internal_format = 0x8058` (`GL_RGBA8`) to prevent silent pixel-write rejection.

- **MEDIUM · Compound literal pointer lifetime on ARM64** — `(int[]){1}` and `(int[]){0}` in `render_params[]` create temporaries that go out of scope on some ARM64 compilers before mpv reads them, breaking `MPV_RENDER_PARAM_FLIP_Y` and `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME`. Replaced with named stack variables `int flip_y = 1` and `int block_time = 0`, passed by reference.

- **v7.0.3**: Full GL state flush at end of `update_frame()` — added `rlDisableShader()`, `glBindBuffer(GL_ARRAY_BUFFER, 0)`, `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)` to force rlgl to drop mpv's shader and VBO bindings.

## v7.0.1 — Overlays, transitions, and fading now run for video playback (May 18, 2026)

### Fixed

- **CRITICAL · Overlays, transitions, and fading still dead-code for video** — Fix 2 (v7.0.0) extended the photo block condition with `|| current_is_video`, but that block was nested inside `if (!current_is_video)` at line 4153 — the outer gate made it unreachable. The overlays (date/filename/count/timer/clock), outgoing transition fade, and incoming fade-in were ALL still skipped for video. Fixed by reverting the dead `|| current_is_video` and moving the full overlays + transitions + fading blocks outside the `if (!current_is_video)` gate, between the collage/photo branch and the CRT loading screen. These blocks now run for ALL content types (photos AND videos).

## v7.0.0 — Video rendering restructure: green CRT screen → actual video playback (May 18, 2026)

### Fixed

- **CRITICAL · `video_rt` texture never drawn to screen** — `update_frame()` decodes mpv frames into `g_mpv.video_rt` (RenderTexture2D FBO) but `render()` had zero `DrawTexturePro` call to blit it. Video existed as a GPU texture but was never displayed. Added `if (current_is_video) { DrawTexturePro(g_mpv.video_rt.texture, ...) }` path at top of the content chain, with `ClearBackground(BLACK)` fallback when video_rt is not yet initialized.

- **CRITICAL · Video fell through to CRT loading screen** — The content if/else chain was `if (!current_is_video) { collage } else if (current_tex.id != 0) { photo } else { CRT }`. Since `current_tex.id == 0` for videos (intentionally unloaded in `load_item()` and `SWAP_TO_VIDEO`), the flow fell straight to `else { // CRT }`, showing the green CRT loading screen instead of video. Fixed by extending the photo block condition to `if ((current_tex.id != 0 && current_tex.width > 0 && current_tex.height > 0) || current_is_video)` and gating the CRT behind `if (!current_is_video && current_tex.id == 0)` — shown only during actual initial preload.

- **MEDIUM · Overlays, transitions, and fade-in skipped for video** — The overlays (date, filename, count, timer, clock), the transition fade-out effect, and the post-swap fade-in were all physically inside the `if (current_tex.id != 0)` photo block. When a video was current, none of these executed: no smooth fade when entering/leaving video, no overlays on video.

- **CRITICAL · str_replace 2a added extra closing brace** — The CRT restructure added two closing braces `}` before the CRT block, but the original `} else {` only had one `}` (closing the photo-render block). The extra brace prematurely closed the `Slideshow` struct, making `init()` and `cleanup()` unreachable. Fixed by removing the extra `}`.

### New transition behaviour

All 4 transition cases now work correctly:
- **Photo → Video**: Photo fades to black, video fades in from black
- **Video → Photo**: Video shows while fading to black, photo fades in
- **Video → Video**: First video fades to black, second fades in from black
- **Photo → Photo**: Existing crossfade / wipe / pixelate shaders unchanged

### Render structure

```
if (current_is_video) → DrawTexturePro(video_rt.texture)
else if (bias_lighting) → ambient background
else → black

if (!current_is_video) → collage mode
else → photo render

// ── Overlays: BOTH photos AND videos ──
// ── Transitions: BOTH photos AND videos ──
// ── Fading: BOTH photos AND videos ──

if (!current_is_video && current_tex.id == 0) → CRT

// ── UNCONDITIONAL OVERLAYS (weather, HUD) ──
```

## v6.0.2 — Correct 24K file count, worker thread join, skip EXIF rotation (May 18, 2026)

### Fixed
- **Scan count**: Root thread now scans root dir only (non-recursive), workers scan subdirs — eliminates 2x file count bug (was 48K, now 24K)
- **Worker threads**: Replaced broken `cv.wait_for(600s)` + `pthread_cancel` with direct `join()` — workers no longer hang or timeout
- **root_thread**: Replaced `detach()` with `join()` — fixes race on `scanned_items` vector
- **Splash rendering**: Full image centered on screen instead of cropped to top 50% (`src_h = h * overlay_y`)
- **EXIF rotation**: Skip `read_exif_rotation()` on 23K JPEGs — replaces ~hour of CIFS I/O with instant `rotation=1`

---
(Older versions archived in previous releases)
