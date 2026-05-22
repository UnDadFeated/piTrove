# Changelog

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
- **CMakeLists.txt** — Proper SDL2_image/png16/jpeg/webp/tiff/heif/SQLite3 linking via pkg-config.

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
- **Video OSD text bar removed** — Removed `--osd-status-msg`, `--osd-align-*`, and `--osd-font-size` arguments. The text overlay rendered a dark bar even when no video was playing. Videos play clean fullscreen.
- **Subtitle overlay removed** — Added `--no-sub` to prevent hardcoded subtitles from rendering over video content.

## v8.3.0 — 30% video ratio, OSD progress, splash fallback, 5-day scan (May 20, 2026)

### Added
- **30% video ratio** — Videos now play 3 per cycle of `videos_per_photos` items (default 10 = 30%). Replaced hardcoded `10 photos + N videos` with dynamic `photos_per_cycle = v_pp - 3`. Configurable via `videos_per_photos` (1–100).
- **Splash fallback chain** — If `splash_file` is empty or path not found, searches: `src/splash.png` → exe dir → `/proc/self/exe` resolution → parent `src/` dir. Falls back to solid dark background if none found.

### Changed
- **Scan window reduced** — `scan_window_days` default changed from `15` to `5` (configurable). Cuts scan time from ~5 min to ~2.5 min, reduces cache from 45K to ~12K items.
- **Video OSD moved to bottom** — mpv OSD now shows `filename.ext - MM:SS` bottom-left (native mpv rendering). Removed redundant in-process filename overlay during video playback.
- **Fade-in skipped for videos** — mpv renders natively to DRM; Raylib fade-in would cover mpv output.

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
- **EGL context management removed** — No more `make_egl_current()`, `release_egl_current()`, `mpv_render_context`, FBO params, or `eglGetProcAddress` for mpv. DRM master is dropped before `fork()` and re-acquired on subprocess exit.
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
