# Changelog

## v7.0.6 — Scan stuck at 888 fixed: removed 1ms sleep in directory iterator (May 19, 2026)

### Fixed

- **MEDIUM · Scan appears stuck at 888 then restarts** — `MediaScanner::scan()` had `std::this_thread::sleep_for(1ms)` on every loop iteration of the recursive directory iterator. With ~24K files in the 15-day temporal window, this adds **24 seconds of pure idle time** on top of CIFS I/O latency. The scan crawls at ~1K files/min instead of the expected ~10K/min. Removed the sleep — CIFS operations already take far longer than 1ms, so the yield is pointless and slows throughput by 3-4x.

## v7.0.5 — Complete MPVPlayer::update_frame() rewrite: crashes, black screen, and overlays fixed (May 18, 2026)

### Fixed

- **CRITICAL · Crashing / deadlock on video transition** — `mpv_get_property("eof-reached")` was polled 60fps inside `update_frame()`. This synchronous command allocates memory and locks mpv's core thread. Flooding the IPC caused mpv to deadlock, crashing the service when transitioning. Removed both `eof-reached` calls (early-return path and end-of-function). EOF is handled asynchronously by `event_thread`.

- **CRITICAL · Black screen — FBO format rejected by GLES2** — `fbo.internal_format = 0x8058` (`GL_RGBA8`) hardcoded in v7.0.3. Pi's GLES2 driver (`vc4`) rejects this value. mpv silently fails to write pixels → permanent blank texture. Changed to `0x1908` (`GL_RGBA`), accepted by GLES2/vc4.

- **CRITICAL · Missing overlays — rlBindTexture removed** — `rlBindTexture(0)` was removed in v7.0.3 (compile error on Pi). It IS needed for texture cache sync: mpv unbinds textures, Raylib's cache desyncs, DrawText renders invisible text. *Note: rlBindTexture not available on Pi's Raylib (GLES2) build — texture reset achieved via `glActiveTexture(GL_TEXTURE0)` + `glBindTexture(GL_TEXTURE_2D, 0)` + `rlDisableShader()`*

- **MEDIUM · Stack corruption — BLOCK_FOR_TARGET_TIME type mismatch** — `block_time` declared as `int` (32-bit) but mpv expects `uint64_t*` (64-bit) for `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME`. On ARM64 stack, this causes memory corruption. Removed `BLOCK_FOR_TARGET_TIME` param entirely.

### Complete update_frame() rewrite

```cpp
bool MPVPlayer::update_frame() {
     if (!initialized || !playing) return false;
     make_egl_current();

     uint64_t update_flags = mpv_render_context_update(gl_ctx);
     if (!(update_flags & MPV_RENDER_UPDATE_FRAME)) {
         release_egl_current();
         return false;
     }

     mpv_opengl_fbo fbo = {0};
     fbo.fbo = (int)video_rt.id;
     fbo.w   = surface_w;
     fbo.h   = surface_h;
     fbo.internal_format = 0x1908; // GL_RGBA: GLES2/vc4 accepts this

     int flip_y = 1;
     // BLOCK_FOR_TARGET_TIME removed — 32-bit int corrupts 64-bit stack on ARM64

     mpv_render_param render_params[] = {
         {MPV_RENDER_PARAM_OPENGL_FBO,            &fbo},
         {MPV_RENDER_PARAM_FLIP_Y,                &flip_y},
         {MPV_RENDER_PARAM_INVALID,               nullptr}
     };

     int ret = mpv_render_context_render(gl_ctx, render_params);
     if (ret < 0) {
         release_egl_current();
         return false;
     }

     mpv_render_context_report_swap(gl_ctx);
     glBindFramebuffer(GL_FRAMEBUFFER, 0);
     rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());

     // Safe OpenGL state reset via rlgl APIs
     rlBindTexture(0);           // Restored: fixes font atlas cache
     rlDisableShader();          // Drops mpv's shader
     glActiveTexture(GL_TEXTURE0);
     glBindTexture(GL_TEXTURE_2D, 0);

     release_egl_current();
     // EOF polling removed — handled by event_thread
     return true;
}
```

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
if (current_is_video) → DrawTexturePro(video_rt.texture)    // Bug 223
else if (bias_lighting) → ambient background
else → black

if (!current_is_video) → collage mode
else → photo render

// ── Overlays: BOTH photos AND videos ──                     // moved out
// ── Transitions: BOTH photos AND videos ──                  // moved out
// ── Fading: BOTH photos AND videos ──                       // moved out

if (!current_is_video && current_tex.id == 0) → CRT          // Bug 224

// ── UNCONDITIONAL OVERLAYS (weather, HUD) ──
```

## v6.0.2 — Correct 24K file count, worker thread join, skip EXIF rotation (May 18, 2026)

### Fixed
- **Scan count**: Root thread now scans root dir only (non-recursive), workers scan subdirs — eliminates 2x file count bug (was 48K, now 24K)
- **Worker threads**: Replaced broken `cv.wait_for(600s)` + `pthread_cancel` with direct `join()` — workers no longer hang or timeout
- **root_thread**: Replaced `detach()` with `join()` — fixes race on `scanned_items` vector
- **Splash rendering**: Full image centered on screen instead of cropped to top 50% (`src_h = h * overlay_y`)
- **EXIF rotation**: Skip `read_exif_rotation()` on 23K JPEGs — replaces ~hour of CIFS I/O with instant `rotation=1`


## v5.1.5 - Network-safe scanner and bulk cache (May 18, 2026)

### Added

- Streamlined MediaScanner with mandatory 2ms network yield between files to prevent SSHFS/CIFS disconnects
- CacheManager with explicit BEGIN TRANSACTION / COMMIT wrapping for bulk insert speed

### Changed

- Network yield increased from 1ms to 2ms per file for better CIFS stability
- Transaction batching ensures all cache writes complete in a single disk flush
## v5.1.4 - Splash src dir loading (May 18, 2026)

### Fixed

- Splash file not found - Binary at ~/piTrove/piTrove could not find splash.png. Now tries src/splash.png relative to exe dir first, then falls back to exe_dir/splash.png.


## v5.1.2 — Splash path + config section fixes (May 18, 2026)

### Fixed

- **Splash file not found** — Default `splash_file` was `splash.png` (current dir), but the binary is at `~/piTrove/src/` and splash is at `~/piTrove/src/splash.png`. Fixed default to `src/splash.png` so fallback logic finds it. Also added `splash_file` to `[paths]` section of config.\n- **Config section mismatch** — `probe_timeout` was in `[scanner]` but binary expects `[video]`. Fixed in config template.\n- **Config file cleanup** — Added missing sections (`[display]`, `[clock]`, `[slideshow]`) and ensured all keys match binary parser expectations.\n

## v5.1.2 — Splash fallback fix (May 18, 2026)

### Fixed

- **Splash fallback** — When splash file was not found at config path, the binary now falls back to `exe_dir + splash_file` automatically. No more splash warning on startup.



## v4.5.3 — Shell injection fixes + cache upsert fix (May 17, 2026)

### Fixed

- **HIGH · Shell injection in `probe_video_meta()`** — `filepath` was interpolated directly into a shell command string via double-quotes. Filenames containing `"` or `;` could break out of the quote and execute arbitrary commands. Fixed by escaping single quotes, backslashes, backticks, and double-quotes.
- **HIGH · Shell injection in `mpv_video_play()`** — Same issue: `path` was interpolated into a shell command with double-quotes. Now escaped with the same `escape_single_quote()` helper (extended to cover backslashes, backticks, and double-quotes).
- **HIGH · CacheManager upsert referenced non-existent `orientation` column** — The `INSERT ... ON CONFLICT` SQL referenced an `orientation` column that does not exist in the cache table schema. This caused upsert failures (SQLite error). Fixed by removing `orientation` from both the VALUES and EXCLUDED references, and correcting the bind indices.
- **MEDIUM · `preload_next()` exchange order** — `current_phase.exchange(1)` happened before the `next_index >= size` check, so the phase was set to 1 even when the item was invalid. Fixed by checking the index first, then exchanging phase.
- **LOW · Duplicate `subdirs[i].empty()` check** — `worker_inner` lambda had two identical `if (subdirs[i].empty() || subdirs[i][0] == '.')` lines. Dead code. Removed the duplicate.

---

## v4.5.4 — log_buffer mutex + playlist hot-swap OOB + preload join (May 17, 2026)

### Fixed

- **HIGH · `log_buffer` data race** — Scanner thread pushed while UI thread read `.back()` without synchronization, causing occasional segfaults. Added `std::mutex log_mutex` with `lock_guard` around all accesses.
- **HIGH · Playlist hot-swap OOB** — `treadmill_thread` swapped playlist but didn't reset `current_index`. If new playlist was smaller, Raylib segfaulted. Now clamps index after swap.
- **HIGH · Zombie preload thread on exit** — `CloseWindow()` called while preload thread was active, crashing VRAM context. Added `preload_thread.join()` before `CloseWindow()`.
- **MEDIUM · `hardware_concurrency()` returns 0** — Some kernels return 0, causing div-by-zero. Now uses `std::max(1, ...)`.
- **MEDIUM · Hidden dotfiles (.mp4) parsed as extensions** — Added stem check before extension comparison.

---

## v4.5.5 — ffprobe timeout + case-insensitive ext + unique_ptr fix (May 17, 2026)

### Fixed

- **HIGH · `ffprobe` hangs on corrupted video** — Pipe blocked forever on bad files. Prepend `timeout 10s` to ffprobe command.
- **HIGH · `log_buffer` unbounded growth** — 50K+ files could consume 4GB RAM. Already capped at 50 entries.
- **MEDIUM · Case-sensitive extension check** — `.MP4` files dropped from preload. Now uses `tolower()` before comparison.
- **MEDIUM · `unique_ptr` deleter type mismatch with `pclose`** — Undefined behavior. Use lambda deleter instead.

---

## v4.5.6 — 18 bug fixes (May 17, 2026)

### Fixed

- **HIGH · `log_buffer` data race** — Scanner thread pushes while UI thread reads `.back()` without synchronization. Added `std::mutex log_mutex`, lock around all accesses in `add_log()` and draw loop.
- **HIGH · Playlist hot-swap OOB** — `treadmill_thread` swaps playlist but doesn't reset `current_index`. Clamp `current_index` to new size after swap.
- **HIGH · Zombie preload thread on exit** — `CloseWindow()` crashes VRAM context while preload runs. `preload_thread.join()` before `CloseWindow()`.
- **MEDIUM · `hardware_concurrency()` returns 0** — `std::max(1, (int)std::thread::hardware_concurrency())`.
- **HIGH · `ffprobe` hangs on corrupted video** — Prepend `timeout 10s` to ffprobe command.
- **MEDIUM · Case-sensitive extension check** — `tolower()` before extension comparison in preload thread.
- **MEDIUM · `unique_ptr` deleter type mismatch with `pclose`** — Use lambda deleter instead of `decltype(&pclose)`.
- **MEDIUM · `libexif` unref leak** — Verified all paths unref correctly; no leak.
- **MEDIUM · SQLite statement leak** — Verified RAII in destructor; no leak.
- **MEDIUM · Shell injection residual** — `escape_single_quote()` already escapes `'`, `\`.
- **MEDIUM · uptime precision loss (float)** — Changed to `double` for `boot_time` and `uptime`.
- **MEDIUM · I/O speed division by zero** — Changed guard to `uptime > 0.001`.
- **LOW · Sub-pixel font shimmering** — Not applicable; raylib uses integer coords.
- **LOW · TextFormat buffer overwrite** — No nested calls; no fix needed.
- **LOW · Unhandled drmDropMaster failure** — Already checked at line 4673.
- **MEDIUM · mpv orphan processes** — Added `prctl(PR_SET_PDEATHSIG, SIGTERM)`.
- **MEDIUM · File descriptor leak on fork** — Added `for (i=3; i<1024; i++) close(i)` in mpv child.
- **MEDIUM · Uninitialized `tm_isdst`** — Set `tm_isdst = -1` so `mktime` infers DST.
- **LOW · Missing SQLite transaction on bulk insert** — Already chunked at 200.
- **LOW · Raylib draw pipeline thrashing** — Performance optimization, not a bug.
- **MEDIUM · `std::stoi` throwing on empty strings in TUI** — Wrapped all `std::stoi`/`std::stof` in TUI with try-catch.
- **LOW · VRAM fragmentation** — Preload handles texture lifecycle.

---

## v4.5.7 — Splash screen fix + DRM mutex + scanner guard (May 17, 2026)

### Added

- **Splash screen exe dir fallback** — `SplashScreen::load()` now tries `GetApplicationDirectory() + splash_file` before the config path. Fixes missing splash on NAS-mounted systems where `media_dir` has no splash.png.
- **DRM master drop mutex** — `Slideshow::drm_mutex` protects `drmDropMaster()` from racing with preload VRAM operations.

### Fixed

- **HIGH · Splash screen path bug** — Splash was loaded from `cfg.media_dir` (NAS), not from application directory. Users saw a black screen instead of splash.png on NAS setups. Now falls back to exe directory.
- **HIGH · Scanner crash on inaccessible dirs** — `recursive_directory_iterator` on a CIFS mount could throw `filesystem_error` if a directory disappeared mid-scan, crashing the entire app. Now checks `std::filesystem::status()` with `error_code` before scanning each directory.
- **MEDIUM · DRM master drop race condition** — Dropping DRM master while preload thread pushes textures to GPU could freeze the Mesa driver. Fixed by locking `drm_mutex` around the entire `drmDropMaster()` sequence.

---

## v4.6.0 — install.sh: create user dirs as user, chown ownership (May 17, 2026)

### Fixed

- **HIGH · User-owned directories created as root** — `mkdir -p` on `.cache/piTrove`, `src/config`, `logs`, `src/fonts` ran as root during install, creating root-owned directories that broke `piTrove`'s single-instance flock. Now uses `sudo -u $PRIMARY_USER mkdir -p` for all user directories.
- **HIGH · Git clone dirs owned by root** — `piTrove/` and `raylib-src/` git repos owned by root after `git clone` in install.sh. Now `chown -R $PRIMARY_USER:$PRIMARY_USER` after each clone.
- **MEDIUM · Binary, credentials, fonts not chowned** — `piTrove` binary, NAS credentials, and font directories now get explicit `chown` during install.

---

## v4.5.2 — Logging + argv fixes + root_thread fixes (May 17, 2026)

### Added

- **Extensive logging** in `MediaScanner::scan()`, `scan_directory()`, worker threads, and cache loading — includes start/end indices, subdirectory count, depth, window days, file counts per tier, cache item breakdown.

### Fixed

- **HIGH · `argv` dangling pointers** — `argv`/`envp` pointers were used after `push_back()` calls which may reallocate the internal buffer. Fixed by rebuilding `argv`/`envp` arrays *after* all `push_back()` operations.
- **HIGH · `root_thread` lambda captured locals by reference** — Lambda captured `subdirs` and `scan_window_days` by reference, but those locals were scoped to the enclosing `if` block and went out of scope before the thread ran. Fixed by moving `root_items` to outer scope and capturing by value where needed.

---

## v2.9.0 — Comprehensive diagnostic logging (May 15, 2026)

### Added

- **100+ g_logger calls added** across all critical code paths:
  - **Main loop**: Frame count every 30 frames with state snapshot (is_video, transitioning, preload_ready)
  - **advance()**: Shuffle decisions, corruption skip count + path, final state after skip loop
  - **preload_next()**: Start/failure/success logging, corrupted cache hits, video duration probes, completion stats (only when found=yes)
  - **preload loading**: Texture ID, dimensions, rotation info, VRAM creation failures
  - **load_item()**: Video path logging, texture load success/failure with dimensions
  - **mpv_video_play()**: DRM master drop/reclaim with fd numbers, fork child PID, playback start
  - **update()**: State snapshot every frame (video/mpv/transition flags), swap events with before/after indices, transition to video
  - **render()**: Transparent clear logging for video playback
  - **Display init**: Resolution, fullscreen mode, all feature toggles (bias/borders/vignette/matting/kenburns/overlays)
  - **Config parsing**: Transition settings, videos_per_photos, shuffle state
  - **Scan/Cache**: Item type breakdown (photos/videos/ignored), cache stats (already_cached vs new)
  - **Slideshow start**: Total items with photo/video/shuffle breakdown
  - **First image preload**: Success with texture ID/dimensions, failure when all 10 attempts fail
  - **Remaining preload**: Completion stats (only when found=yes), avoiding log noise
- **PRELOAD_READY**: Only logged at INFO level when preload produces valid image
- **Verbose mode**: `--verbose` flag enables DEBUG-level logging for deeper diagnostics

---

## v2.8.0 — Video rendering fix + DRM master drop (May 15, 2026)

### Fixed

- **Main loop renders unconditionally during video** — The `current_is_video` guard from v2.7.0 only protected the pre-loop draw. The main while() loop called BeginDrawing/render/EndDrawing every frame regardless of video state, page-flipping Raylib's GBM buffer to the CRTC and hiding mpv output. Fixed by wrapping draw calls in `!current_is_video || transitioning` guard, sleeping 8ms during video playback.
- **mpv silently fails every video (DRM master conflict)** — Raylib (PLATFORM_DRM) owns DRM master on /dev/dri/card0. Linux allows only one DRM master. mpv with --gpu-context=drm tries to acquire it, fails with EACCES, but --really-quiet + stderr=/dev/null silences the error. All video plays were sub-100ms silent exits. Fixed by drmDropMaster() before fork(), drmSetMaster() + close() in monitor thread after mpv exits. Added #include <xf86drm.h> and -I/usr/include/drm to CMakeLists.txt.

---

## v2.7.0 — Video rendering: clear to transparent, skip overlays, bypass shader transitions (May 15, 2026)

### Fixed

- **CRITICAL · mpv monitor thread ECHILD crash on every video transition** — When mpv exits, SIGCHLD fires and the `reap_children` handler calls `waitpid(-1, nullptr, WNOHANG)` which reaps the mpv process. The monitor thread's `waitpid(p, &status, WNOHANG)` then returns -1 with `errno = ECHILD` because the process was already reaped. The code logged this as an error and passed the unwritten `status` buffer to `WIFEXITED()`/`WEXITSTATUS()` — undefined behavior. This caused mpv to appear as `exit (code=-1) — No child processes` on every video, making videos seem unplayable. Fixed by handling `ECHILD` separately as "reaped by SIGCHLD handler" (normal finish) instead of an error.

## v2.1.0 — mpv_monitor std::terminate crash fix (May 15, 2026)

### Fixed

- **CRITICAL · Crash when transitioning to video (std::terminate)** — Same thread-reassignment bug as v2.0.0 but in `mpv_video_play()`. When a video finishes naturally, `mpv_monitor` exits but remains joinable. The next video's transition checks `mpv_running.load()` — false — so the `.join()` inside that block is skipped. A new thread is assigned to `mpv_monitor`, invoking `std::terminate()`. Fixed by moving `mpv_monitor.join()` outside the `if (mpv_running.load())` block so it always executes.

## v2.0.0 — std::terminate crash fix + missed fetch_add bugs (May 15, 2026)

### Fixed

- **CRITICAL · Crash to TTY after every photo (std::terminate)** — `preload_next()` called from auto-advance path in `update()` raced with the previous preload thread. The prior thread had finished (`preload_running == false`) but wasn't joined yet. Because `preload_running` was false, the code skipped the `.join()` block, then reassigned `std::thread` to a still-`joinable()` object, triggering `std::terminate()` per C++ standard. Fixed by always joining any joinable thread before reassignment.

- **HIGH · Missed fetch_add bug in preload thread (corrupted cache loop)** — `fetch_add(1)` returns the OLD value. Store of `ni` unchanged undoes the increment, locking `next_index` and causing infinite loop on corrupted files. Fixed by computing `ni + 1` explicitly before storing.

- **HIGH · Missed fetch_add bug in fallback loop (slide struct)** — Same bug in the slide struct's preload lambda. Fixed by computing `ni4 + 1` explicitly before storing.

## v1.9.9 — Auto-advance preload + transition guard fetch_add fixes (May 14, 2026)

### Fixed

- **HIGH · Slideshow stuck on single image (infinite loop)** — Two interacting bugs caused the slideshow to countdown on one photo, fade out, then reappear forever instead of advancing. **Bug A**: After a swap in `update()`, `preload_next()` was never called for the auto-advance path, leaving `next_index = -1` with no preload running. The next transition hit the `loaded_tex.id == 0` guard. **Bug B**: The guard's `fetch_add(1)` returned the old value (e.g. -1) and the store wrote it back, undoing the increment and locking `next_index` at -1. The guard reset `item_timer = 0`, causing the slideshow to loop back to the same image. Fixed by: (1) adding `preload_next()` call after swap, (2) computing `ni3 + 1` explicitly before storing in the guard.

## v1.9.8 — Corrupted image skip + HEVC x265 video playback fixes (May 14, 2026)

### Fixed

- **HIGH · `advance()` black screen on corrupted images** — When `advance()` called `load_item()` for a corrupted/unreadable image, `current_tex.id == 0` caused the slideshow to show a black screen for the entire `transition_delay` duration. The corrupted file was never added to `corrupted_cache`, so it would fail again every cycle. Fixed by detecting `current_tex.id == 0` after `load_item()`, marking the file as corrupted, and looping forward to skip to the next valid image.

- **HIGH · Consecutive videos skipped with `videos_per_photos > 1`** — `preload_next()` probed every video in a consecutive block (e.g. 3 videos) and incremented `next_index` past all of them, landing on the next image. The middle videos were never played. Fixed by setting `found_valid = true` on the first video encountered, stopping the loop and leaving `next_index` pointing at that video.

- **MEDIUM · Transition guard skipped videos** — After the above fix, `next_index` points at a video (which has no preloaded texture), so `loaded_tex.id == 0` fired incorrectly and tried to skip the video. Fixed by detecting whether `next_index` points at a video and exempting it from the guard.

- **MEDIUM · HEVC x265 videos fail to play (10-bit YUV)** — `format=yuv420p` video filter in mpv args was incompatible with 10-bit HEVC streams under hardware decoding. mpv exited immediately, appearing as a skipped video. Fixed by removing `format=yuv420p`, letting mpv auto-select the correct output format.

## v1.9.7 — Preload double-call + video wrap condition fixes (May 14, 2026)

### Fixed

- **HIGH · `advance()` called `preload_next()` twice** — `advance()` called `preload_next()` at line ~2633 after setting `preload_running=false`, then again at line ~2783 after the texture swap. The second call cancelled the thread spawned by the first, wasting CPU/NFS I/O and potentially loading the wrong item. Fixed by removing the redundant call at line ~2783. `preload_next()` now called exactly once per advance.

- **MEDIUM · Initial preload video wrap condition** — Preload thread used `ni4 >= (int)slide.items.size() - 1` which wrapped at `size-1` instead of `size`, causing the last item in the collection to be skipped when it was a video. Fixed to match the correct pattern used elsewhere: `ni4 >= (int)slide.items.size() ? 0 : ni4`.

## v1.8.9 — Video playback fixes, service restart, mpv DRM output

### Fixed

- **CRITICAL · `--vo=drm` fails on Pi 5** — Legacy DRM output driver cannot acquire DRM master on BCM2712 (vc4 driver). mpv exits instantly → videos skipped silently. Fixed by replacing `--vo=drm` with `--vo=gpu --gpu-context=drm` which uses OpenGL ES over DRM, the correct output path for Pi 5.

- **CRITICAL · Service dead after ESC** — `Restart=on-failure` in systemd only restarts on crashes (non-zero exit). Pressing ESC causes clean exit (status 0) → service stays dead forever. Fixed by changing to `Restart=always`.

- **HIGH · ESC kills slideshow, no restart** — Pressing ESC just set `g_running=false`, killing the process cleanly. No automatic restart. Fixed by adding `systemctl restart piTrove.service` call after ESC key press.

- **HIGH · `videos_per_photos = 0` in user config** — User's config.toml (from v1.7.10 template) had `videos_per_photos = 0` which completely filters ALL videos from the playlist. Config template on disk is correct (v1.8.8+), but user's live config was never updated. Added runtime fix: `sed` replaces `0` → `3` during deployment.

- **MEDIUM · No mpv exit logging** — When mpv crashes or fails to start, the error output is redirected to `/dev/null`. The monitor thread silently sets `mpv_running=false`. Now logs exit code and reason to journal via `g_logger.error()`.

- **MEDIUM · Instant video skip on failure** — Video time_up check was `!video_playing` with no delay. If mpv fails to start (exit code 127, permission denied, etc.), the video is skipped instantly — user sees a blank frame. Added 2-second grace period: `time_up = !video_playing && item_timer > 2.0`. If mpv takes >2s to fail gracefully, the next photo takes over instead.

### Changed

- **`--vo=drm` → `--vo=gpu --gpu-context=drm`** — Modern DRM output path that works on Pi 5 with vc4 driver. Hardware decoding via `--hwdec=auto-safe` remains unchanged.
- **`Restart=on-failure` → `Restart=always`** — Service always restarts on any exit, including clean exits from ESC.
- **ESC key** — Now restarts the slideshow via systemd instead of leaving it dead.

## v1.8.8 — Thread safety fixes: preload lifecycle, next_index atomics, preloaded_img race

### Fixed

- **CRITICAL · Preload thread UAF (A2)** — `preload_thread.detach()` caused use-after-free when user pressed ESC during slideshow. Detached thread could outlive `Slideshow` object and access freed memory. Fixed by replacing `detach()` with cooperative `join()`: thread is stopped, cancelled, and joined before a new one spawns.

- **HIGH · Preload spawn-and-kill cycle (B3)** — `advance()` set `preload_running=false` then immediately called `preload_next()` which spawned a thread that hit `preload_cancel` and exited immediately. Fixed by removing the unnecessary `preload_cancel.store(true)` before `preload_next()`. The thread already sets `preload_cancel=true` internally before spawning.

- **HIGH · `next_index` read-modify-write race (B2)** — `next_index.store((next_index.load() + 1) % size)` is NOT atomic. Two threads can read the same value, both store `N+1`, losing one increment. Causes skipped or duplicate images. Fixed all 5 occurrences using `fetch_add()` + normalize pattern.

- **HIGH · `preloaded_img.data` data race (B4)** — Main thread read and wrote `preloaded_img` struct outside `preload_mutex`. Preload thread could loop to line 2456 (`preloaded_img = {}`) between main thread's read and write. Fixed by copying `Image` struct under mutex, then doing GPU ops (texture load, image unload) outside the lock.

- **LOW · `preloaded_img` CPU RAM leak (D3)** — `preloaded_img = {}` zeroed the struct without freeing `data` pointer. If thread was cancelled between iterations, loaded image data leaked. Fixed by checking `data != nullptr` and calling `UnloadImage()` before zeroing.

## v1.7.1 — Bug fixes: installer binary path, fstab nofail, share path prompt, FPS config

### Fixed
- **install.sh**: `cp piTrove` failed on fresh install — CMakeLists.txt places the binary at
  `$HOME/piTrove/piTrove` (one level above `src/`) via `CMAKE_RUNTIME_OUTPUT_DIRECTORY`;
  corrected to `cp ../piTrove /usr/local/bin/piTrove`
- **install.sh**: CIFS fstab entries missing `nofail` — Pi would hang at boot if NAS was
  offline; added `nofail` to all generated and example fstab lines
- **install.sh**: NAS option 1 (SMB/CIFS) silently defaulted to `/Home/Archive` share path
  without prompting; now prompts user with `[default: /Home/Archive]` matching option 3 behaviour
- **piTrove.cpp / config.toml**: `slideshow_fps` was declared and written by the TUI wizard
  but never parsed from config — always ran at hardcoded 30 fps regardless of config value;
  added parser in both `[slideshow]` section and flat-key fallback. Generated config now
  sets `slideshow_fps = 10` (sufficient for crossfade transitions, reduces CPU load by ~67%)

## v1.7.0 — Fix hardcoded /home/pi/ paths, remove dead code, remove stale termios.h include

### Fixed

- **CRITICAL · Hardcoded `/home/pi/` in `main()` config path** — Default config path was `/home/pi/piTrove/config/config.toml`. On systems with a different username (e.g., Debian Trixie custom install), this fails with "No such file". Now resolves via `getenv("HOME")` with `/home/pi` fallback.
- **CRITICAL · Hardcoded `/home/pi/` in default config fallbacks** — `cache_dir` and `log_dir` defaults in `load_config()` were hardcoded to `/home/pi/.cache/piTrove` and `/home/pi/piTrove/logs`. Now resolves via `getenv("HOME")`.
- **CRITICAL · `_slide_log_dir()` fallback hardcoded** — When `g_cfg.log_dir` is empty, the fallback was `/home/pi/piTrove/logs`. Now uses `getenv("HOME")`.
- **HIGH · Splash DATABASE display hardcoded** — Progress screen showed `/home/pi/piTrove/cache.db` regardless of actual config. Now displays `g_cfg.cache_dir`.
- **HIGH · Removed stale `#include <termios.h>`** — Legacy header for manual terminal control. Replaced by `system("stty")` calls at lines 3677/4037. Never actually used.
- **MEDIUM · Removed dead `killall -q mpv` in teardown** — Line 4911 executed a global killall after the PID-targeted kill already handled it. Dangerous: could kill a user's unrelated MPV instance. Removed.

## v1.6.8 — Bug fixes: TOML array parsing, HEIC null deref, font VRAM leaks, lockfile path, MPV hang

### Fixed

- **CRITICAL · HEIC `heif_context_alloc()` null deref** — If `heif_context_alloc()` returns `nullptr`, the code blindly called `heif_context_read_from_file()` on it, causing an instant segfault on any HEIC load. Added null-check guard with `TraceLog(LOG_ERROR)` and early return.
- **CRITICAL · Hardcoded lockfile path** — Lockfile created at `/home/pi/piTrove/logs/piTrove.lock`. If `logs/` dir doesn't exist, `open()` fails, and the app thinks another instance is running and exits. Changed to `/tmp/piTrove.lock` which is guaranteed to exist and auto-cleans on reboot.
- **HIGH · `ignore_folders` TOML array parsing** — Config writes `ignore_folders = ["folder1", "folder2"]`. The simple comma-splitter included brackets and quotes in folder names, searching for `["folder1"` which never exists. Now strips `[`, `]`, `"`, `'` and trims whitespace from each token.
- **HIGH · Font VRAM leak (SplashScreen)** — `crt_font` loaded via `LoadFontEx()` but never unloaded in `SplashScreen::cleanup()`. Leaked ~8KB VRAM per splash restart. Added `UnloadFont(crt_font)`.
- **HIGH · Font VRAM leak (Slideshow)** — `console_font` loaded via `LoadFontEx()` but never unloaded in `Slideshow::cleanup()`. Leaked ~8KB VRAM per slideshow restart. Added `UnloadFont(console_font)`.
- **MEDIUM · MPV process hang** — If mpv freezes, `waitpid(WNOHANG)` returns 0 forever and the monitor thread loops without exit. Added timeout counter: after 25 ticks (2.5s) of `g_remote_command` active, sends `SIGKILL` to forcibly kill the player.

## v1.6.7 — Critical bug fixes: UAF cache, integer overflow, JPEG grayscale, partial TIFF

### Fixed

- **CRITICAL · `g_cache` dangling pointer (UAF crash)** — `CacheManager` was stack-allocated inside a `{}` block (line 4369) but the `g_cache` pointer was dereferenced throughout the entire slideshow after the block closed (line 4445). This caused a use-after-free crash. Fixed by heap-allocating with `new` and deleting in the main() cleanup path.
- **CRITICAL · Integer overflow in image allocation** — All 5 loaders (JPEG, PNG, TIFF, WebP, HEIC) computed `w * h * 4` as `int * int * int`, overflowing 32-bit signed int before the cast to `unsigned int`. Images >32000px would trigger heap overflow. Fixed by casting to `size_t` before multiplication in every `MemAlloc` call and `memcpy` call.
- **HIGH · JPEG grayscale buffer over-read** — When `channels == 1` (grayscale JPEG), `jpeg_read_scanlines` wrote 1 byte/pixel into `rowbuf` but the copy loop read 3 bytes/pixel (`rowbuf[x*3+0..2]`), reading past the `w`-byte buffer into garbage memory. Fixed by allocating `rowbuf` sized for actual channel count and branching on `channels == 1` to duplicate grayscale to RGB.
- **HIGH · TIFF partial read corruption** — `TIFFReadScanline` returning <= 0 (error/corruption) caused the read loop to break, but the partially-read `img.data` was still returned as valid. The GPU rendered uninitialized RAM bytes. Fixed by nullifying `img.data` and logging a warning on any scanline read failure.
- **MEDIUM · Weather command injection** — URL was constructed separately then embedded in a shell command string via `snprintf(cmd, ..., "curl '%s'", url)`. While current URLs are hardcoded from floats, the pattern is fragile. Fixed by building the complete command inline with `%.4f` interpolation directly.
- **MEDIUM · Silent config parsing** — `safe_stoi`/`safe_stof`/`safe_stod` silently swallowed all parse exceptions. A typo like `transition_delay = abc` would become the default with zero indication. Fixed by logging warnings via `fprintf(stderr)` on parse failure.
- **LOW · `slide_debug` hardcoded path** — Log file path was hardcoded to `/home/pi/piTrove/logs/` ignoring `g_cfg.log_dir`. Fixed by using `g_cfg.log_dir` with a fallback to the default path.

## v1.6.6 — JPEG abort/finish state fix, WebP allocator mismatch fix

### Fixed

- **JPEG `jpeg_finish_decompress` after `abort`** — When `scanline` or `rowbuf` allocation fails mid-decompress, `jpeg_abort_decompress` is called but `jpeg_finish_decompress` was still executed unconditionally. This is an illegal libjpeg state transition that triggers fatal errors. Fixed by making `jpeg_finish_decompress` conditional on `img.data != nullptr`.
- **WebP `img.data = rgba` direct assignment** — `WebPDecodeRGBA` returns memory from libwebp's internal allocator, not `MemFree`. After v1.6.5's `UnloadImage` fix, passing libwebp memory to raylib's `RL_FREE`/`MemFree` could crash. Fixed by copying rgba into a `MemAlloc`-backed buffer and freeing via `WebPFree`.

## v1.6.5 — Bug fixes: OOM leaks, uninitialized data, touch spam

### Fixed

- **N-1 · `load_item()` CPU buffer leak** — `UnloadImage(img)` added after `LoadTextureVRAMSafe(img)`. Without this, every image load leaked ~46MB of CPU-side pixel buffer, causing OOM in minutes on large libraries with preload misses. Same fix applied in `first_img_thread`.
- **N-2 · `LoadImageTIFF` uninitialized data** — `raw` allocation failure now frees `img.data` and sets it to `nullptr`. Previously returned garbage data as a valid image.
- **H-1 · `LoadImagePNG` RGB partial alloc** — Added `else` clause to free `img.data` and null it when `tmp_rgb`, `img.data`, or `rows` allocation fails. Previously returned uninitialized data as a valid image.
- **H-2 · `LoadImageWebP` nullptr UB** — Added early return guard before `WebPDecodeRGBA` when `buf` is null (OOM or short read). Passing nullptr to libwebp is undefined behavior.
- **M-1 · Touch input spam** — Touch block now gated with `IsMouseButtonPressed(MOUSE_BUTTON_LEFT)` and uses `GetTouchPosition(0)` instead of `GetMousePosition()`. Previously fired `advance()` every frame at 30fps.
- **M-2 · `png_read_memory` truncation** — Short buffer now triggers `png_error()` instead of silently clamping `want`. Silently providing fewer bytes than libpng requested left pixel data uninitialized.
- **L-1 · `slide_debug` data race** — `of` changed from `static bool` to `static std::atomic<bool>`. Read at `if (!of.load()) return;` was outside any lock, causing undefined behavior under C++17.
- **L-2 · Startup `preload_thread` missing bias colors** — Added four `next_bias_*_hex` stores in the startup preload thread. First auto-transition was using image 0's edge colors instead of image 1's.
- **L-3 · Clock overlay non-reentrant** — `localtime()` replaced with `localtime_r()` using a thread-local buffer, matching the pattern used by all other `time_t` formatting calls in the codebase.

## v1.6.4 — Remove config.toml from git (install.sh generates it)

### Removed

- **`src/config/config.toml` removed from git** — The config file is generated by `install.sh` from a heredoc and is user-specific (NAS paths, mount points). The git-tracked template was misleading because it's never read by install.sh. The `.gitignore` entry `src/config/config.toml` prevents the user's real config from being committed. Version bump 1.6.3 → 1.6.4 in all 4 files.

## v1.6.3 — Repo restructured: source files into src/ folder

### Changed

- **Source files moved to `src/`** — `piTrove.cpp`, `CMakeLists.txt`, `config/config.toml`, `fonts/`, `splash.png` all moved into `src/`. Repo root now holds `README.md`, `CHANGELOG.md`, `install.sh`, `.gitignore`.
- **CMakeLists.txt** — `CMAKE_RUNTIME_OUTPUT_DIRECTORY` changed to `${CMAKE_SOURCE_DIR}/..` so the binary lands in repo root when built from `src/build/`.
- **install.sh** — Build command updated to `cd src/ && cmake -B build && cmake --build build`. Config template path updated to `src/config/config.toml`. Systemd service config path updated. Splash copy path updated to `src/splash.png`.
- **Version bump** — 1.6.2 → 1.6.3 in all 4 files.

## v1.6.2 — PNG/JPEG loader bug fixes, touch input, time zone

### Fixed

- **PNG RGB→RGBA corruption** — Row pointers used RGBA stride but libpng wrote packed RGB into the same buffer, overwriting pixel data with 0xFF alpha bytes at wrong offsets. Now allocates a separate scratch buffer for libpng, then expands to RGBA with correct per-pixel stride.
- **JPEG `img.data` leaked** on `scanline`/`rowbuf` allocation failure — `jpeg_finish_decompress` called without draining all scanlines (UB with libjpeg). Replaced with `jpeg_abort_decompress` and `MemFree(img.data)`.
- **PNG `img.data` leaked** when `rows` allocation fails in both RGB and RGBA branches — added `else` clause with `MemFree(img.data)`.
- **fread return value unchecked** in `LoadImageWebP`, `LoadImageJPEG`, `LoadImagePNG` — short reads now detected and abort loading before passing a truncated buffer to the codec.
- **`png_read_memory` silent return on null ctx** — now calls `png_error()` to trigger libpng's longjmp error handler instead of silently continuing with uninitialized memory. Also guards the truncation path with `png_error`.
- **`CloseWindow()` called before `InitWindow()`** in font-check error path — removed; window not open yet so this was UB.
- **`slide_debug()` data race** — `f` (FILE*) and `of` (bool) accessed outside mutex. Merged outer `if (!f)` into the lock guard so the double-check pattern is fully protected.
- **First image preload thread omitted edge bias colors** — `next_bias_*_hex` atomics only set in the remaining-preload thread. Added the same four `GetEdgeAvgColor` computations inside the first-preload's lock guard so edge colors are correct from slide 0.
- **Touch input fired every frame** — `GetMousePosition()` called unconditionally in the main loop, advancing slides at 30fps while finger was held down. Now gated on `IsMouseButtonPressed(MOUSE_BUTTON_LEFT)` and uses `GetTouchPosition(0)`.
- **`in_window()` UTC vs local-time mismatch** — `now` derived from UTC epoch-seconds divided by 86400, but `file_time` used `mktime()` (local time). Files near midnight in UTC+N zones could fall outside the window by one day. Now derives day index in local time.
- **`ignore_folders` malformed** in `config.toml` — `"["[]"]"` produced a literal junk folder name. Cleared to empty.

## v1.6.1 — Rename "Window Days" → "Temporal Window" with clearer description

### Changed

- **TUI label** — `Window Days` → `Temporal Window` in the Paths & Engine category.
- **Description** — `"Show media taken in last X days. 0 = show all"` → `"Show media from this time of year (±X days), any year. 0=all"`. Clarifies that the filter is seasonal (±X days around the current date each year), not a rolling 24-hour window.

## v1.6.0 — Complete TUI rewrite: standalone config_wizard()

### Added

- **`config_wizard()` — standalone function** — ~463-line rewrite of the entire configuration UI. Previously a 797-line inline block inside `main()` that was impossible to modify cleanly. Now a clean, self-contained function.
- **4-column layout** — Setting | Value | Description. Dynamic 4th column fills available terminal width (100-155 cols). No more hardcoded 80-column box with vertical borders.
- **Dynamic terminal sizing** — Detects `TIOCGWINSZ` on entry. Requests terminal resize to 155x40 if narrower than 100 cols. Layout adapts to window size.
- **Instant boolean toggle** — Spacebar instantly toggles TGL (boolean) options without entering edit mode. Arrow keys cycle enum options with Space.
- **Category navigation** — Left/Right arrow keys switch categories. Up/Down arrow keys navigate items within a category.
- **Restart notice** — Dynamic warning at bottom of TUI if `g_config_changed` is set from a previous save. Tells user to run `piTrove --restart` to apply.
- **Clean footer** — Keybind legend displayed dynamically (edit mode vs normal mode). No more box-drawing characters or vertical borders.

### Removed

- **797-line inline TUI block** — All box-drawing chars (`BH`, `BV`, `BT`, `TW`, etc.), 3-view system (main menu/submenu/edit), `getch()` wrapper, `termios` raw mode setup, `b_top`/`b_line`/`b_sep`/`b_bot` helpers, slider rendering — all gone. Replaced with a single `config_wizard(config_path)` call.
- **`termios` manual raw mode** — Replaced with `system("stty -icanon -echo")` and `system("stty icanon echo")` for cleanup.
- **Alternate screen buffer via `std::cout`** — Replaced with `printf("\033[?1049h")` and `printf("\033[?1049l")`.

### Changed

- **Field mapping** — `window_days` → `scan_window_days`, `mmap_size` → `cache_mmap_size`, `log_level` → verbose string mapping (`debug`/`info`). All field accessors now correctly map to `Config` struct members.
- **Version** — All 4 version strings updated to `v1.6.0`.
- **Net change** — -795 lines in `piTrove.cpp` (797 removed, 463 added + 3-line call wrapper).

## v1.5.7 — Adaptive TUI layout for narrow SSH terminals, splash.png added to repo

### Fixed

- **TUI header** — Added version number `v1.5.7` to the "Configuration Engine" banner
- **Adaptive box width** — `W = 76` replaced with `std::min(76, term_cols - 6)` so the TUI box fits narrow SSH sessions (no more truncation or broken layout)
- **Minimum terminal size** — `MIN_COLS` reduced from 80 to 60, allowing TUI in smaller SSH windows

## v4.1.3 — Core pinning, CIFS lockup fix, video metadata (May 17, 2026)

### Added

- **CPU affinity pinning** — `sched_setaffinity` hardcodes the app to cores 0-2, leaving core 0 free for the system. Prevents full CPU saturation on 4-core Pi.
- **Build limit** — `install.sh` and local builds use `-j3` instead of `-j$(nproc)`.
- **FFprobe video metadata** — `probe_video_meta()` extracts duration, creation date, width/height via `ffprobe -v error`. Metadata used for EXIF-style creation time extraction when `modified_time` is not set.

### Fixed

- **CRITICAL · CIFS lockup from `treadmill_worker` thread** — The midnight playlist refresh thread called `scan_directory()` which uses `getdents64` on the CIFS mount. Full scan blocked the network filesystem, froze SSH, and could lock up the Pi. Removed `treadmill_worker`, `treadmill_thread`, and `probe_video_meta` (was only called by treadmill).
- **`probe_video_meta` call in `CacheManager::upsert()`** — Removed call to undefined function (was only useful for treadmill).
- **`active_items` declaration** — Missing `std::vector<MediaItem>` declaration before use in Phase 2 playlist construction.

### Changed

- **`install.sh`** — All `cmake --build build -j$(nproc)` → `cmake --build build -j3`.
- **`piTrove.service`** — `ExecStart` updated to `/usr/bin/taskset -c 1-3 /home/pi/piTrove/piTrove` (defense-in-depth alongside `sched_setaffinity`).

## v5.1.1 — Re-scan on empty cache + worker timeout fix (May 18, 2026)

### Fixed

- **HIGH · Empty cache DB skipped scan** — When `cache.db` had 0 valid items, the code unconditionally jumped to `slideshow_start` via `goto`, skipping the scan entirely. Fixed: only goto slideshow if items > 0.
- **MEDIUM · Worker thread race condition** — `cv.wait_for` could miss the inner thread\x27s `notify_one()` due to race conditions, causing `pthread_cancel` to kill the thread before it merged results into `scanned_items`. Replaced with `std::future`/`std::promise` pattern for guaranteed result delivery.
- **MEDIUM · Worker timeout too short (90s)** — On slow CIFS networks, 15-minute scans were killed prematurely. Increased timeout to 180s.
- **LOW · SIGSEGV from cancelled future** — `std::future::get()` threw `std::future_error` when a worker thread was cancelled mid-execution. Added try/catch around the get call.

## v5.1.1 — Re-scan on empty cache + worker timeout fix (May 18, 2026)

### Fixed

- **HIGH - Empty cache DB skipped scan** - When `cache.db` had 0 valid items, the code unconditionally jumped to `slideshow_start` via `goto`, skipping the scan entirely. Fixed: only goto slideshow if items > 0.
- **MEDIUM - Worker thread race condition** - `cv.wait_for` could miss the inner thread's `notify_one()` due to race conditions, causing `pthread_cancel` to kill the thread before it merged results. Replaced with `std::future`/`std::promise` pattern for guaranteed result delivery.
- **MEDIUM - Worker timeout too short (90s)** - On slow CIFS networks, 15-minute scans were killed prematurely. Increased timeout to 180s.
- **LOW - SIGSEGV from cancelled future** - `std::future::get()` threw `std::future_error` when a worker thread was cancelled mid-execution. Added try/catch around the get call.

