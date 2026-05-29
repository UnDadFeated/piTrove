# piTrove Bug & Vulnerability Tracker

This file tracks the active bugs, resource safety concerns, and boundary checks analyzed under the continuous quality improvement loop.

## Batch #20: Concurrency, Thread Safety & Cooldown Persistence (Complete)

- [x] **Bug #1 (Severity: High)** — `http_server.cpp` — Detached thread in `trigger_motion` handler runs `mqtt_publish` on exit, causing race and potential segfault.
- [x] **Bug #2 (Severity: High)** — `mqtt.cpp` — Detached thread spawned on EVERY `mqtt_publish` call, creating thread exhaustion and exit races.
- [x] **Bug #3 (Severity: Medium)** — `mqtt.cpp` — Non-atomic read/write of `g_screen_blanked` in HA state publishing and direct non-store assignments.
- [x] **Bug #4 (Severity: Medium)** — `util.cpp` — Detached thread in `set_display_power` running `vcgencmd` on every call, causing exit races.
- [x] **Bug #5 (Severity: High)** — `overlay.cpp` — Missing `font_loaded` or `overlay_font` null pointer safety checks in `OverlayManager::draw` causing crashes if no monospace font is found.
- [x] **Bug #6 (Severity: High)** — `util.cpp` — Double locking deadlock on `__slide_debug_mtx` inside `slide_debug` on rotate check immediately hangs the slideshow.
- [x] **Bug #7 (Severity: High)** — `transition.cpp` — Missing null checks for `prev_tex` and `next_tex` inside `TransitionEngine::render` causing crash loops on skips.
- [x] **Bug #8 (Severity: High)** — `http_server.cpp` — Detached thread spawned per HTTP request in `server_loop` causes undefined behavior on exit.
- [x] **Bug #9 (Severity: Medium)** — `scanner.cpp` — Missing null check on `now` returned by `localtime_r` in `is_in_seasonal_window`.
- [x] **Bug #10 (Severity: Medium)** — `main.cpp` — Missing null check on `last_check_tm` returned by `localtime_r` in `watchman_loop`.
- [x] **Feature Request (Severity: High)** — `main.cpp` — Ensure shown history cooldowns are persistent across app restarts even during directory scanning phases.

## Batch #19: Concurrency, Socket, Process & Memory Safety Hardening (Complete)

- [x] **Bug #1 (Severity: High)** — `main.cpp` — Concurrency Data Race on `g_scanned_items` and `g_eligible` erases.
- [x] **Bug #2 (Severity: High)** — `main.cpp` — Concurrency Race in `advance_playlist` execution.
- [x] **Bug #3 (Severity: High)** — `main.cpp` — Concurrency Race in twin-portrait playlist initialization.
- [x] **Bug #4 (Severity: High)** — `image_loader.cpp` — Missing `entry->data` null check in EXIF reader.
- [x] **Bug #5 (Severity: Medium)** — `scanner.cpp` — Broken `/proc/self/fd` optimization always triggering slower fallback.
- [x] **Bug #6 (Severity: Medium)** — `mpv_player.cpp` — Pause state SIGTERM ignored by suspended child processes.
- [x] **Bug #7 (Severity: Medium)** — `tui.cpp` — Division by zero in enums toggling under empty vector constraints.
- [x] **Bug #8 (Severity: Low)** — `config.cpp` — Unclamped `bias_strength` boundary validation.
- [x] **Bug #9 (Severity: Medium)** — `http_server.cpp` — Unbounded socket write hang (missing `SO_SNDTIMEO`).
- [x] **Bug #10 (Severity: Low)** — `http_server.cpp` — Unsafe SoC temperature & DB size substring formatting.
- [x] **Bug #11 (Severity: Low)** — `main.cpp, scanner.cpp` — Signed integer overflow in directory date parsing.

## Batch #18: Concurrency, Thread Safety & Graphics Hardening (Complete)


- [x] **Bug #1 (Severity: High)** — `renderer.cpp` — Contiguous flat memcpy in draw_blurred_from_raw() without pitch row-alignment can cause memory corruption or visual distortion.
- [x] **Bug #2 (Severity: High)** — `renderer.cpp` — Contiguous flat memcpy in load_splash() without pitch row-alignment can cause memory corruption or visual distortion.
- [x] **Bug #3 (Severity: Medium)** — `preload.cpp` — PreloadQueue queue capacity blockage from stale thread-decodes pushing behind epoch cancellations.
- [x] **Bug #4 (Severity: Medium)** — `renderer.cpp` — Potential EGL DRM master drop locks when page flips are queued. Requires explicit EGL client synchronization.
- [x] **Bug #5 (Severity: Low)** — `blur.cpp` — Missing boundary check for scale/division-by-zero negative casts in downsample_image().
- [x] **Bug #6 (Severity: Low)** — `renderer.cpp` — Unchecked SDL_GetError() logging and stale SDL errors inside rendering context.
- [x] **Bug #7 (Severity: Low)** — `tui.cpp` — Dirty card rendering size mismatch under low terminal rows/cols boundary sizing.
- [x] **Bug #8 (Severity: Low)** — `config.cpp` — Missing validation constraints on border_width settings causing matte overlap.
- [x] **Bug #9 (Severity: Low)** — `main.cpp` — Missing watchman loop check for directory permission failures on metadata reload.
- [x] **Bug #10 (Severity: Low)** — `mpv_player.cpp` — Redundant DRM connector logging on secondary connector queries.

## Batch #17: Concurrency, Performance & Signal Safety Hardening (Complete)

- [x] **Bug #1 (Severity: High)** — `main.cpp, http_server.cpp` — Missing SIGPIPE signal ignore. Client disconnecting during dynamic preview stream crashes the whole slideshow application.
- [x] **Bug #2 (Severity: High)** — `image_loader.cpp, preload.cpp` — Contiguous memcpy in SDL_CreateSurface without validating pitch row-alignment can cause visual corruption or OOB crash.
- [x] **Bug #3 (Severity: Medium)** — `mpv_player.cpp` — Thread block in stop() while holding mtx lock freezes the main presentation loop during video termination.
- [x] **Bug #4 (Severity: Medium)** — `util.cpp` — Synchronous vcgencmd display_power fallback command blocking in main rendering thread causes lag spikes on keypress.
- [x] **Bug #5 (Severity: Medium)** — `mqtt.cpp` — Synchronous mosquitto_pub system() execution delays status publishing on slow connections.
- [x] **Bug #6 (Severity: Medium)** — `scanner.cpp` — Indefinite waitpid subprocess block on stuck network mount during ffprobe timeout.
- [x] **Bug #7 (Severity: Medium)** — `http_server.cpp` — Unbounded detached client thread spawning risks file descriptor exhaustion under socket floods.
- [x] **Bug #8 (Severity: Low)** — `image_loader.cpp` — Missing validation of zero width/height image dimensions causing division-by-zero NaN scaling.
- [x] **Bug #9 (Severity: Low)** — `util.cpp` — Negative snprintf return check in Logger to prevent index arithmetic errors.
- [x] **Bug #10 (Severity: Low)** — `cache.cpp` — Incomplete SQLite Transaction rollbacks leaving locks active on failure.

## Batch #15: Reliability, Concurrency & Performance Hardening (Complete)

- [x] **Bug #1 (Severity: High)** — `http_server.cpp:30` — Non-atomic `g_listen_fd` read/write data race and file descriptor reuse race.
- [x] **Bug #2 (Severity: Medium)** — `transition.cpp:253` — GPU dissolve transition CPU bound drawing loop (fixed alongside Batch #16 Bug #4).
- [x] **Bug #3 (Severity: Low)** — `util.cpp:251` — Logger silent log truncation and redundant double formatting. Removed redundant level check in `log()`, clamped snprintf truncation instead of silent drop.
- [x] **Bug #4 (Severity: Medium)** — `mpv_player.cpp:130` — Unwritable child `mpv` logs location `/home/pi/mpv_debug.log` inside container.
- [x] **Bug #5 (Severity: High)** — `preload.cpp:309` — Preloader CPU waste on main thread (fixed alongside Batch #16 Bug #1).
- [x] **Bug #6 (Severity: Medium)** — `cache.cpp:229,235` — Unchecked SQL transactional failures in `begin_transaction()` and `commit_transaction()`.
- [x] **Bug #7 (Severity: Low)** — `install.sh:642` — Stray bracket character `}` in branch selection dialog display formatting.
- [x] **Bug #8 (Severity: Medium)** — `http_server.cpp:923,924` — Modifying config port `g_cfg.http_port` without signaling `g_config_changed`.
- [x] **Bug #9 (Severity: Low)** — `scanner.cpp:65` — Safe checks for string manipulation inside `file_ext` utility.
- [x] **Bug #10 (Severity: High)** — `main.cpp:953,999` — Corrupt cache database remove/re-scan doesn't release old SQLite handles before deleting the file.
- [x] **Feature Request (Severity: Medium)** — `config.cpp, tui.cpp, main.cpp` — `reset_cooldown_on_restart` was already fully implemented in earlier revisions.

## Batch #16: Thread Safety, Performance & Signal Safety Hardening (Complete)

- [x] **Bug #1 (Severity: High)** — `preload.cpp:130-146` — Edge strip pixel sampling and edge color extraction moved to worker thread. `PreloadedItem` stores precomputed edge data; `try_dequeue()` copies values directly without per-pixel sampling.

- [x] **Bug #2 (Severity: Medium)** — `http_server.cpp:849-851` — TOCTOU race in screen toggle fixed with `compare_exchange_weak` loop for atomic read-modify-write.

- [x] **Bug #3 (Severity: Medium)** — `mqtt.cpp:50-53` — Removed detached thread; `mqtt_publish()` calls `::system()` synchronously, eliminating shutdown race on captured string data.

- [x] **Bug #4 (Severity: Medium)** — `transition.cpp:256-268` — Dissolve transition uses `static thread_local std::vector<SDL_FRect>` buffer to avoid per-frame heap allocation churn.

- [x] **Bug #5 (Severity: Medium)** — `font_render.cpp:91-96` — Text cache eviction changed from full clear to single-entry LRU eviction (erase oldest entry).

- [x] **Bug #6 (Severity: Medium)** — `util.cpp:108-126` — `terminate_handler()` now uses async-signal-safe `write()` + `open()` sysfs approach instead of `set_display_power()` which called unsafe `g_logger.info()` and `::system()`.

- [x] **Bug #7 (Severity: Low)** — `main.cpp:179-226` — `calculate_fit_rect_in_area()` now has a synchronisation comment pointing to `Renderer::calculate_fit_rect()`.

- [x] **Bug #8 (Severity: Low)** — `scanner.cpp:34-35` — Dead `get_dents64()` function removed.

- [x] **Bug #9 (Severity: Low)** — `blur.cpp:163` — Redundant `std::max(src.width, 1)` replaced with plain `src.width`.

- [x] **Bug #10 (Severity: Low)** — `cache.cpp:158-159` — `sqlite3_column_int64` clamped to `int` range via `std::min(col, (int64_t)INT_MAX)` before assignment.

## Batch #14: System Safety, Memory & Concurrency Hardening (Complete)

- [x] **Bug #1 (Severity: High)** — `image_loader.cpp:136` — Top-Edge column average out-of-bounds read if image height < 3px.
- [x] **Bug #2 (Severity: High)** — `mqtt.cpp:252,267` — Undefined behavior / double close of `g_mqtt_fp` in MQTT subscriber loop and shutdown.
- [x] **Bug #3 (Severity: High)** — `renderer.cpp:321` — High-frequency background CPU-GPU VRAM texture thrashing.
- [x] **Bug #4 (Severity: Medium)** — `scanner.cpp:284` — Folder seasonal window scanner bypasses filtering on continuous YYYYMM/YYYYMMDD directory formats.
- [x] **Bug #5 (Severity: Medium)** — `main.cpp` — Missing configuration hot reload check in main slideshow loop.
- [x] **Bug #6 (Severity: High)** — `font_render.cpp:32` — Premature `TTF_Quit()` shutdown crashes multi-context text renderers.
- [x] **Bug #7 (Severity: Medium)** — `blur.cpp:120` — High heap memory box-blur allocations due to hardcoded max dimension.
- [x] **Bug #8 (Severity: Medium)** — `transition.cpp:211` — Pixelate transition CPU choking from 130,000 individual `SDL_FRect` render calls.
- [x] **Bug #9 (Severity: High)** — `preload.cpp:232` — Stale preload decodes bypass `cancel_all` filters and deadlock preloader queue.
- [x] **Bug #10 (Severity: Low)** — `tui.cpp:219,610` — Uninitialized stack garbage sizing calculations when TUI run headlessly.

## Batch #13: System Stability, Thread-Safety & Performance Optimizations (Complete)

- [x] **Bug #1 (Severity: High)** — `util.cpp:623` — Unsafe `std::localtime` in `get_modified_time_date()` is not thread-safe. Multiple threads calling it concurrently race on the static buffer, corrupting date classifications.
- [x] **Bug #2 (Severity: High)** — `http_server.cpp` — Single-threaded blocking socket processing. Server blocks synchronously during connection accept, reads, and multi-megabyte streams, freezing remote control and integrations on slow connections.
- [x] **Bug #3 (Severity: High)** — Multiple Files — Broken proprietary `vcgencmd` display commands inside container. Calls to `system("vcgencmd ...")` fail because the utility is missing in standard Debian container.
- [x] **Bug #4 (Severity: Medium)** — `font_render.cpp:61` — High-frequency GPU texture thrashing. Dynamic overlays (clock, count) re-render CPU surfaces and allocate new GPU textures on every frame (60 FPS), bottlenecking CPU-GPU bus.
- [x] **Bug #5 (Severity: Medium)** — `scanner.cpp:105` — Broken date parser in `is_in_seasonal_window()`. Sequential digit consumption parses continuous `YYYYMMDD` formats as a single huge number, bypassing seasonal window filter for all such files.
- [x] **Bug #6 (Severity: Low)** — `tui.cpp:65,77` — Unused static input helper functions `peek_input` and `parse_escape_seq` trigger compiler `-Wunused-function` warnings.
- [x] **Bug #7 (Severity: Low)** — `tui.cpp:762` — Misleading footer loop indentation triggers GCC compiler `-Wmisleading-indentation` warning.
- [x] **Bug #8 (Severity: Low)** — `cache.cpp:239` — Incomplete database schema verification skips validation of the recently added `is_camera` column.
- [x] **Bug #9 (Severity: Low)** — `util.cpp:178` — Conditional console output flush in `Logger::flush_loop` delays console logging when log file creation/open fails.
- [x] **Bug #10 (Severity: Medium)** — `util.cpp:255` — Unchecked `snprintf` return value in `Logger::log` risks stack underflow/overflow if format size is negative or too large.

## Batch #5: Configuration Validation Boundary Audits (Complete)

- [x] **Item 1 (Severity: Medium)** — Enforce safe boundary constraints for `max_concurrent` threads in `config.cpp` to prevent resource starvation or out-of-memory crashes on extremely high or negative values.
- [x] **Item 2 (Severity: Low)** — Enforce minimum positive speed boundary for `ken_burns_speed` to prevent division-by-zero or dynamic animation rendering stalls in the presentation engine.
- [x] **Item 3 (Severity: Low)** — Cap the maximum `matting_size` matte frame border size to prevent screen overflow or negative boundary artifacts during collage composites.
- [x] **Item 4 (Severity: Medium)** — Enforce a minimum boundary limit for `transition_delay` (slideshow interval) to prevent rapid slideshow thread cycles and preloader thrashing.
- [x] **Item 5 (Severity: Medium)** — Limit the maximum and minimum boundaries for `probe_timeout` to safeguard the ffmpeg/ffprobe subprocess reapers from indefinite freezes on corrupt digital frame containers.
- [x] **Item 6 (Severity: High)** — Safely constrain `mmap_size` (SQLite memory mapping size) to prevent allocation failures on low-resource ARM64 board models.
- [x] **Item 7 (Severity: Low)** — Enforce logical boundaries for `scan_window_days` seasonal search spreads to prevent invalid negative filters.
- [x] **Item 8 (Severity: Low)** — Guard the maximum subdirectory scanning depth `scan_depth` against excessive recursion stack overflows.
- [x] **Item 9 (Severity: Low)** — Clamp dynamic photo `cooldown_days` boundaries safely to prevent negative cooldown constraints.
- [x] **Item 10 (Severity: Low)** — Clamp auto-brightness parameters (`brightness_auto_min` and `brightness_auto_max`) strictly within valid physical backlight percentages (0% to 100%).

## Batch #6: Thread Safety & Resource Leak Audits (Complete)

- [x] **Bug #1 (Severity: High)** — `scanner.cpp:160-195` — fork() child leaks /proc/self/fd DIR handle to ffprobe process. The /proc/self/fd directory handle opened in the child process is never closed, leaking a file descriptor to the ffprobe process.
- [x] **Bug #2 (Severity: High)** — `mqtt.cpp:109-204` — `start_mqtt_client()` spawns a detached thread that popen()s mosquitto_sub. Fixed: replaced `std::thread::detach()` with tracked `std::thread` + `stop_mqtt_client()` joinable shutdown. Added `g_mqtt_fp` mutex-protected FILE pointer for clean `pclose()` on exit.
- [x] **Bug #3 (Severity: Low)** — `http_server.cpp:373` — DASHBOARD_HTML hardcodes version "v11.0.0" in the subtitle, never updates to current VERSION. Fixed: replaced `static const std::string DASHBOARD_HTML` with `get_dashboard_html()` function that replaces placeholder with `VERSION` at runtime.
- [x] **Bug #4 (Severity: Medium)** — `renderer.cpp:633` vs `overlay.cpp:24` — FontRenderer is double-allocated: once in `Renderer::load_splash()` line 633 and once in `OverlayManager::init()` line 24. Both are independent FontRenderer instances but this wastes ~200KB VRAM per overlay. (Skipped: intentional architecture — Overlay and Renderer use separate FontRenderer instances for thread-safe rendering separation.)
- [x] **Bug #5 (Severity: Low)** — `main.cpp:748` — `usleep(800000)` is POSIX-only and deprecated in favor of C++11 `std::this_thread::sleep_for(std::chrono::microseconds(800000))`. Fixed: replaced all `usleep()` calls with `std::this_thread::sleep_for()` across main.cpp, mpv_player.cpp, and tui.cpp.
- [x] **Bug #6 (Severity: High)** — `main.cpp:1518-1523,1572-1576` — `playlist_lock.unlock()` followed by background preloader dequeue, then `playlist_lock.lock()` creates a race window where `advance_playlist()` or watchman thread can modify `g_eligible` while the preloader is dequeuing by path, potentially invalidating references. Fixed: captured paths under lock, performed all I/O (preload dequeue + fallback sync load) outside lock, re-locked only for metadata updates. Eliminated unlock/relock pattern entirely.
- [x] **Bug #7 (Severity: High)** — `util.cpp:76` — `crash_handler()` calls `::system("vcgencmd display_power 1")` which is NOT async-signal-safe per POSIX. Should use `write()` syscalls only. Fixed: replaced with direct `/sys/class/graphics/fb0/blank` sysfs write (O_WRONLY, write(), close()) — fully async-signal-safe.
- [x] **Bug #8 (Severity: Medium)** — `preload.cpp:118-174` — Edge strip sampling reads uninitialized pixels when image width/height is < 3px. The inner loop `for (int d = 0; d < 3; d++)` accesses pixel offsets that may be out of bounds. Fixed: added guard for images < 3px; clips sample count to actual dimension before edge sampling.
- [x] **Bug #10 (Severity: High)** — `main.cpp:141-175` — `should_be_twin_portrait()` accesses `g_eligible` and `current_idx` without holding `g_playlist_mtx`, creating a data race with watchman thread and playlist modifications. Fixed: changed signature to `should_be_twin_portrait(std::vector<MediaItem>&, int idx)` — all callers now pass `g_eligible` by reference under lock context.

## Batch #7: Data Races & Non-Atomic Atomic Accesses (Complete)

- [x] **Bug #1 (Severity: High)** — `http_server.cpp:930` — `g_screen_blanked = !g_screen_blanked.load()` is a non-atomic read-modify-write on `std::atomic<bool>`. Fixed: uses `g_screen_blanked.exchange()` for atomic toggle.
- [x] **Bug #2 (Severity: High)** — `http_server.cpp:939` — `g_screen_blanked = false` is a non-atomic assignment to `std::atomic<bool>`. Fixed: uses `g_screen_blanked.store(false)`.
- [x] **Bug #3 (Severity: High)** — `main.cpp:1310,1325,1342` — `g_screen_blanked = false/true` non-atomic assignments in SDL event handlers and motion cooldown. Fixed: replaced all direct assignments with `.store()` and `.exchange()`.
- [x] **Bug #4 (Severity: Medium)** — `http_server.cpp:933,942,944` — `g_cfg.mqtt_topic_prefix` and `g_cfg.mqtt_motionsensor_topic` read without holding `g_config_mtx`, creating a data race with config reload. Fixed: copied config values under lock before use.
- [x] **Bug #5 (Severity: Medium)** — `http_server.cpp:945-948` — detached lambda in trigger_motion handler reads `g_cfg` fields directly. Fixed: config values captured by value before thread spawn.
- [x] **Bug #6 (Severity: High)** — `mqtt.cpp:16-42` — `mqtt_publish()` reads `g_cfg.mqtt_enabled`, `g_cfg.mqtt_broker`, `g_cfg.mqtt_user`, `g_cfg.mqtt_pass`, `g_cfg.mqtt_port` without holding `g_config_mtx`. Fixed: all config values copied under lock before command construction.
- [x] **Bug #7 (Severity: Medium)** — `http_server.cpp:683` — `g_cfg.cache_dir` read without lock in API status endpoint. Fixed: copied under lock.

## Batch #8: Unlocked g_cfg Reads in MQTT & Main Loop (Complete)

- [x] **Bug #1 (Severity: Medium)** — `mqtt.cpp:57` — `publish_ha_discovery()` reads `g_cfg.mqtt_topic_prefix` without `g_config_mtx`. Fixed: copied under lock.
- [x] **Bug #2 (Severity: Medium)** — `mqtt.cpp:110-114` — `publish_ha_discovery()` reads `g_cfg.mqtt_motionsensor_topic` without lock. Fixed: copied under lock.
- [x] **Bug #3 (Severity: Medium)** — `mqtt.cpp:125` (now 130) — `start_mqtt_client()` reads `g_cfg.mqtt_enabled` without lock. Fixed: copied under lock before early return.
- [x] **Bug #4 (Severity: Low)** — `main.cpp:1280` — `g_cfg.transition_effect` read without lock at startup. Fixed: copied under lock.
- [x] **Bug #5 (Severity: Low)** — `main.cpp:1459` (now ~1463) — `g_cfg.transition_duration` and `g_cfg.ken_burns_zoom` read without lock in main loop. Fixed: copied under lock.

## Batch #9: Unlocked g_cfg Reads & Non-Atomic in_transaction (Complete)

- [x] **Bug #1 (Severity: Medium)** — `cache.cpp:70` — `g_cfg.cache_mmap_size` read without `g_config_mtx`. Fixed: copied under lock.
- [x] **Bug #2 (Severity: Medium)** — `cache.cpp:174` — `in_transaction` read without `db_mutex` while being written under lock. Fixed: changed `in_transaction` to `std::atomic<bool>` with `.store()`.
- [x] **Bug #3 (Severity: Low)** — `main.cpp:1842` — `g_cfg.transition_delay` read without lock in main loop. Fixed: copied under lock.

## Batch #10: g_scanned_items Data Race (Complete)

- [x] **Bug #1 (Severity: Medium)** — main.cpp:1398-1401 vs main.cpp:667-668 — Main loop modifies g_scanned_items (erase) while holding g_playlist_mtx, but the watchman thread at line 667 reads g_scanned_items for filter_playlist() and at line 668 reads g_scanned_items.size() — both WITHOUT g_playlist_mtx. Fixed: watchman now acquires g_playlist_mtx during the entire midnight playlist re-filtering and swapping block to prevent concurrent access data races.

## Batch #11: Resolution-Independent Scaling (Complete)

- [x] **Task #1 (Severity: Low)** — `renderer.h` — Add `scale_px(base_px_1080p)` helper to `Renderer` class that converts pixel values defined for 1080p to the current screen resolution (`base_px * screen_w / 1920`).
- [x] **Task #2 (Severity: Low)** — `renderer.cpp:270`, `main.cpp:196` — Apply `g_renderer.scale_px()` to `matting_size` so it scales from 96px@1080p to proportionally larger at 4K.
- [x] **Task #3 (Severity: Low)** — `renderer.cpp:426`, `main.cpp:271/343/1783/1851` — Apply `g_renderer.scale_px()` to `glow_depth` so it scales from 86px@1080p. All `draw_bias_lighting` callers updated.
- [x] **Task #4 (Severity: Low)** — `preload.cpp:304` — Apply `g_renderer.scale_px()` to `blur_radius` so it scales from 14@1080p.
- [x] **Task #5 (Severity: Low)** — `main.cpp:197/271/343/1782/1848` — Apply `g_renderer.scale_px()` to `border_width` in all config snapshots.

## Batch #21: TUI, Scanner, Thread Safety & Signal Safety (Complete)

- [x] **Bug #1 (Severity: High)** — `tui.cpp:610` — Flash message timer uses absolute TUI start time (`now - start_time`) instead of message trigger time, causing all flash messages to disappear after 2 seconds from TUI start regardless of when they were triggered.

- [x] **Bug #2 (Severity: High)** — `tui.cpp:687` — Category bar loop `for(int i=0; i<9; i++)` only renders 9 of 10 categories, making the MQTT category (CATS[9]) invisible and unreachable via TUI navigation.

- [x] **Bug #3 (Severity: High)** — `main.cpp:1657,1664` — `g_eligible[next_idx].type` read after `playlist_lock.unlock()` creates a data race with the watchman thread which may move-replace `g_eligible` with a new vector at line 750, causing use-after-free on the vector's backing store.

- [x] **Bug #4 (Severity: High)** — `main.cpp:1992` — `should_be_twin_portrait(g_eligible, ...)` called without `playlist_lock` may `std::swap` elements of `g_eligible` (line 173), causing a data race with other `g_eligible` readers during preload lookahead.

- [x] **Bug #5 (Severity: High)** — `mpv_player.cpp:54-57` — `play()` calls blocking `waitpid(video_pid, &status, 0)` while holding `mtx` with no SIGKILL fallback timeout, causing permanent deadlock if the mpv child process ignores SIGTERM.

- [x] **Bug #6 (Severity: Medium)** — `main.cpp:427-428` — ON_THIS_DAY branch in `filter_playlist` reads `g_cfg.show_people_faces` and `g_cfg.keep_animals` without `g_config_mtx`. The normal filter path at lines 484-485 correctly snapshots under lock.

- [x] **Bug #7 (Severity: Medium)** — `http_server.cpp:1030-1036` — TOCTOU race in connection limit: `g_active_connections.load() >= 10` check and `fetch_add(1)` are not a single atomic operation, allowing unlimited concurrent connections.

- [x] **Bug #8 (Severity: Medium)** — `http_server.cpp:41-60` — Uncaught exception in tracked thread lambda prevents `finished->store(true)`, leaking the thread handle permanently in `g_http_client_threads`.

- [x] **Bug #9 (Severity: Medium)** — `image_loader.cpp:152-158` — Bottom edge sampling uses `d=-1..+1` but `ry=sh` for `d=+1` is filtered by `ry < sh` bounds check, yielding only 2 samples instead of the intended 3, producing asymmetric edge color estimates vs. the top edge.

- [x] **Bug #10 (Severity: Medium)** — `config.cpp:191` — `g_logger.warn` called during `g_cfg.load()` before `g_logger.init()` at main.cpp:885; unrecognized config key warnings are lost or misrouted.

## Batch #12: Resolution Scaling & Dynamic Glow Settings (In Progress)

- [x] **Task #1 (Severity: Medium)** — `config.h`, `config.cpp`, `tui.cpp` — Added `resolution_preset` (0=720p,1=1080p,2=1440p,3=4K) as ENM in Display TUI category. Sets `screen_w`/`screen_h` dynamically.
- [x] **Task #2 (Severity: Medium)** — `config.h`, `tui.cpp` — `bias_strength` (0-200) exposed in TUI Slideshow category at index 9.
- [x] **Task #3 (Severity: Medium)** — `config.h`, `tui.cpp` — `matte_opacity` (0.05-0.50) exposed in TUI Slideshow category at index 12.
- [x] **Task #4 (Severity: Medium)** — `config.h`, `tui.cpp` — `vignette_strength` (0.10-0.80) exposed in TUI Slideshow category at index 13.
- [x] **Task #5 (Severity: Medium)** — `config.h`, `tui.cpp` — `glow_depth` (16-120) and `blur_radius` (6-24) exposed in TUI Slideshow category at indices 14 and 15.
- [x] **Task #6 (Severity: Low)** — `config.cpp` — `resolution_preset` handler sets `screen_w`/`screen_h` from preset; `scale_px()` in Config struct already uses `screen_w / 1920.0`.
- [x] **Task #7 (Severity: Low)** — `tui.cpp` — `save_cfg()` writes `resolution_preset`, `matte_opacity`, `vignette_strength`, `blur_radius`, `glow_depth`.
- [x] **Task #8 (Severity: Low)** — `tui.cpp` — Display category: Resolution (ENM, index 1). Slideshow category: Bias Strength, Matte Opacity, Vignette Strength, Blur Radius, Glow Depth (5 new items, 14→18 count).


## Batch #22: Pipeline Hang & Splash Stalling (Done 2026-05-28)

### Fixed
- [x] **CIFS file_exists() blocks playlist mutex** — Moved `file_exists()` calls outside `g_playlist_mtx` by unlock-before-I/O pattern. If CIFS `stat()` hangs, the playlist lock is released so the HTTP API can still respond and the slideshow can be skipped/managed remotely.
- [x] **Stale WAL/SHM preventing cache init** — Orphaned SQLite WAL+SHM files from killed metadata-extraction runs caused cache DB to appear locked, stalling the scanning phase with "AWAITING I/O PIPELINE".
- [x] **Fast-path splash flash** — Removed INIT splash with progress=0 before cache check; shows real item count for 800ms instead.
- [x] **No cache-complete visual** — Added final "CACHED: N" frame + 500ms delay after cache build.
- [x] **Metadata extraction reverted** — EXIF/ffprobe in caching loop too slow (6+ min); deferred to post-commit path.

## Batch #23: Warnings, Dead Code & TUI Border Separation (Complete 2026-05-28)

- [x] **Bug #1 (Severity: Low)** — `tui.cpp` — Dynamic flash message timer duration instead of hardcoded 2000 ms.
- [x] **Bug #2 (Severity: Low)** — `tui.cpp` — Safely clamp `cache_mmap_size` in settings input to prevent overflow or out-of-bounds database sizes.
- [x] **Bug #3 (Severity: Low)** — `scanner.h, scanner.cpp` — Cleaned up defunct `get_dents64` prototype and unused `count` parameter from `scan_directory`.
- [x] **Bug #4 (Severity: Low)** — `scanner.cpp` — Uncommented `(void)timeout_ms` to fix compiler warnings.
- [x] **Bug #5 (Severity: Low)** — `main.cpp` — Simplified `scan_directory` caller site signature.
- [x] **Bug #6 (Severity: Medium)** — `main.cpp` — Thread-safe initialization reads of configuration values at startup under mutex.
- [x] **Bug #7 (Severity: Low)** — `renderer.cpp` — Uncommented unused parameter casts to silence compiler warnings.
- [x] **Bug #8 (Severity: Low)** — `renderer.cpp` — Properly close sysfs streams on failures to prevent file descriptor leaks.
- [x] **Bug #9 (Severity: Low)** — `transition.cpp` — Removed unused `screen_w`/`screen_h` parameters from layout math.
- [x] **Bug #10 (Severity: Low)** — `overlay.cpp` — Silenced unused parameter warning with clean casts.
- [x] **Bug #11 (Severity: High)** — `preload.cpp` — Fixed background preloader color-matched matte bug where borders defaulted to solid black.
- [x] **Bug #12 (Severity: Low)** — `image_loader.cpp` — Removed dead `#include <SDL3_image/SDL_image.h>`.
- [x] **Bug #13 (Severity: Medium)** — `mqtt.cpp` — Secure `g_mqtt_fp` allocation to happen inside lock context post popen validation.
- [x] **Task #14 (Severity: Medium)** — `tui.cpp, config.cpp` — Implemented separate user options for `3D Border` and `3D Border Width` in the TUI Display category.

## Batch #24: Hardening & TUI Gaps (Complete 2026-05-29)

- [x] **Bug #1 (Severity: Low)** — `http_server.cpp` — Redundant duplicate assignment setting `max_attempts` twice.
- [x] **Bug #2 (Severity: High)** — `mqtt.cpp` — Publisher thread spawning/joining race condition causing crashes (`std::terminate`) during concurrent re-creation and exits.
- [x] **Bug #3 (Severity: Low)** — `tui.cpp` — Redundant duplicate `#include <chrono>` statement.
- [x] **Bug #4 (Severity: High)** — `tui.cpp` — Hardcoded static category items count cut off `HTTP Timeout` and `HTTP Bind attempts` in `Videos` tab.
- [x] **Bug #5 (Severity: High)** — `tui.cpp` — Hardcoded static category items count cut off `Reset Cooldown` in `Slideshow` tab.
- [x] **Bug #6 (Severity: Medium)** — `tui.cpp` — Added `Max Brightness` integer setting to settings interface, exposing `brightness_auto_max`.
- [x] **Bug #7 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `rotation` value.
- [x] **Bug #8 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `ken_burns_zoom` value.
- [x] **Bug #9 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `ken_burns_speed` value.
- [x] **Bug #10 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `video_volume` value.
- [x] **Bug #11 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `video_probe_timeout` value.
- [x] **Bug #12 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `transition_delay` value.
- [x] **Bug #13 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `transition_duration` value.
- [x] **Bug #14 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `scan_depth` value.
- [x] **Bug #15 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `scan_window_days` value.
- [x] **Bug #16 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `max_concurrent` value.
- [x] **Bug #17 (Severity: Low)** — `tui.cpp` — strict boundary clamping on `brightness_auto_min` value.
- [x] **Bug #18 (Severity: Low)** — `tui.cpp` — strict boundary clamping on new `brightness_auto_max` value.
- [x] **Bug #19 (Severity: Low)** — `tui.cpp` — Removed orphaned invalid enum option for `Clock 24h` toggle settings.
- [x] **Bug #20 (Severity: Low)** — `tui.cpp` — Replaced remaining hardcoded category loops with dynamic array count calculation.

## Batch #25: Hardening, Dead Code Removal & Safety Fixes (Complete 2026-05-29)

- [x] **Bug #1 (Severity: Low)** — Nested duplicate `try/catch` in DRM connector probe.
- [x] **Bug #2 (Severity: Low)** — Consolidated duplicate `photos_per_video` spacing calculation after playlist loop.
- [x] **Bug #3 (Severity: Medium)** — Replaced complex backward modular arithmetic with safe indexing in `advance_playlist`.
- [x] **Bug #4 (Severity: High)** — Added missing null-pointer checks on cache database manager during startup twin portrait loads.
- [x] **Bug #5 (Severity: High)** — Prevented redundant CPU-intensive layout evaluations during active transitions.
- [x] **Bug #6 (Severity: Medium)** — Wrapped cooldown and scan window configuration accesses in playlist filter under config mutex lock.
- [x] **Bug #7 (Severity: Low)** — Added architecture warning docs for directory listing timeouts on standard local/mounted systems.
- [x] **Bug #8 (Severity: High)** — Replaced detached, risk-prone waiting threads for killed child probes with immediate synchronous process reaping.
- [x] **Bug #9 (Severity: Medium)** — Overhauled video probe outputs parsing to be completely robust to carriage return (`\r\n`) formats.
- [x] **Bug #10 (Severity: Low)** — Removed redundant secondary SQLite database busy timeout allocation path.
- [x] **Bug #11 (Severity: Medium)** — Locked cache memory-mapped size reads to prevent data races during database initialization.
- [x] **Bug #12 (Severity: Medium)** — Restructured logging framework rotation logic to properly transition to new timestamped files without stalling.
- [x] **Bug #13 (Severity: Low)** — Cached environment variables lookups to minimize duplicate system calls during logging.
- [x] **Bug #14 (Severity: High)** — Corrected the divisor in the bottom ambient backdrop average calculation to use exact sample counts instead of a hardcoded value.
- [x] **Bug #15 (Severity: Critical)** — Eliminated dangerous double-locking deadlock vulnerabilities inside overlay adaptive text rendering.
- [x] **Bug #16 (Severity: Low)** — Renamed helper layout mathematics function in transitions module to prevent name shadowing.
- [x] **Bug #17 (Severity: Critical)** — Secured MQTT dash system command execution constructs against shell injection vulnerability.
- [x] **Bug #18 (Severity: Low)** — Documented thread lifecycle exit routines for background remote motion timers.
- [x] **Bug #19 (Severity: High)** — Enforced connection-specific socket read/write timeouts on HTTP clients to prevent pool starvation.
- [x] **Bug #20 (Severity: Medium)** — Clamped video volume configuration parser entries to keep them within system-supported boundaries.



## 2026-05-29 Monitoring — v11.5.8
- [ ] Container crash/restart at ~20:31 — only ran 1s, no new log file created. Check for SIGSEGV/SIGABRT or OOM.
