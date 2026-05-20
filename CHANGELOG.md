# Changelog

## v7.1.1 — Stability and Version Bump (May 19, 2026)
- Bumped version to 7.1.1 across all system files.

## v7.1.0 — Concurrency and Memory Hardening (May 19, 2026)
### Fixed
- **CRITICAL · SQLite Concurrency Race Conditions (B282)** — Implemented `SQLITE_OPEN_FULLMUTEX` (serialized handle), `sqlite3_busy_timeout(5000)`, and `std::lock_guard` across all `CacheManager` methods to eliminate segfaults and deadlocks between the UI thread and the background scanner.
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
