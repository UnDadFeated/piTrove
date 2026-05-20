
## Bug Fix Round 20 (242-251)

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| 242 | HIGH | `CacheManager::load_cached` incorrect column indices — duration, exif, bad, last_shown shifted | Corrected indices: duration=2, exif=3, bad=4, last_shown=5, timestamp=6 |
| 243 | HIGH | Race condition in failsafe scan — worker threads detached and `threads_remaining` decremented before work done | Removed inner `worker_thread` creation/detach; perform work directly in lambda |
| 244 | MEDIUM | Memory leak of `CacheManager` — `g_cache` overwritten in Phase 2 if DB already existed | Only create `new CacheManager()` in Phase 2 if `g_cache == nullptr` |
| 245 | HIGH | Data race on `Slideshow::items` shared_ptr access — read without lock in main loop, update, render, advance, preload | Added `get_items()` helper with `shuffle_mutex` lock for all accesses |
| 246 | MEDIUM | Ineffective treadmill cooldown filter — `scan_directory` didn't load `last_shown` from cache | Added `g_cache->load_cached(mi)` to `scan_directory()` |
| 247 | HIGH | Failsafe scan worker threads detached then 'waited' on via joinable() | Fixed by removing detached threads (same as 243) |
| 248 | HIGH | `Slideshow::preload_next` blocks main thread while joining potentially hanging `preload_thread` | Changed to non-blocking: signal cancel and return if running; added check in `update()` to resume preloading |
| 249 | HIGH | Data race on `MediaItem` elements during treadmill shuffle — shuffle happens on active vector | Shuffle `final_playlist` before creating `shared_ptr` and assigning to `slide.items` |
| 250 | MEDIUM | `Slideshow::load_item` accessing `items` without lock when `items_ptr` is null | Used `get_items()` helper to safely capture shared_ptr |
| 251 | LOW | `Slideshow::update` returns immediately on `preload_cancel`, skipping frame updates | Wrapped preload processing in `else` block instead of returning |

## Bug Fix Round 21 (252-261)

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| 252 | HIGH | Heap overflow in image loaders — `size_t` to `unsigned int` truncation in `MemAlloc` | Used `SafeMemAlloc` with `size_t` |
| 253 | HIGH | Race condition in `SplashScreen::render` — `log_buffer` modified while rendering | Copied `log_buffer` under `log_mutex` before render loop |
| 254 | HIGH | Race condition on global `g_cfg` — `load_config` updates without lock | Wrapped `g_cfg` updates and clamping in `g_config_mtx` |
| 255 | HIGH | Blocking CIFS hangs in `MediaScanner::scan` — `std::filesystem` iterators block indefinitely | Replaced with `read_dir` + manual recursion with timeouts |
| 256 | MEDIUM | `scan_directory` depth parameter ignored — `recursive_directory_iterator` scans infinite depth | Implemented manual recursion limit using `depth` |
| 257 | HIGH | `std::terminate()` risk in timeout wrappers — `pthread_detach` used instead of `std::thread::detach()` | Replaced `pthread_detach(t.native_handle())` with `t.detach()` |
| 258 | LOW | `is_month_in_window` potential exception — `std::stoi` on regex match | Wrapped `std::stoi` in try-catch block |
| 259 | LOW | `CacheManager::open` partial failure leak — statements not freed if `open` returns false | Handled by destructor (~CacheManager calls close) |
| 260 | LOW | `MediaScanner` root files scan logic is redundant/clunky | Streamlined root file processing in `scan()` |

## Bug Fix Round 22 (262-271)

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| 262 | HIGH | Use-After-Free / Null Pointer Dereference in `slide_debug` — `fflush` outside lock | Moved `fflush` inside lock in `slide_debug` |
| 263 | HIGH | Out-of-Bounds Access after Playlist Hot-Swap — `current_index` not clamped after swap | Clamped `current_index` in `swap_playlist` |
| 264 | MEDIUM | Data Race on Global Configuration `g_cfg` — reads in `update`/`render` without lock | Captured `g_cfg` locally under `g_config_mtx` in `update()` |
| 265 | LOW | Potential Buffer Overflow / Path Truncation in `MPVPlayer::play` — fixed 4096 buffer | Replaced fixed buffer with `std::string` |
| 266 | LOW | Misleading Success Return in `MPVPlayer::play` — returns true even on `mpv_command` failure | Now returns result of `mpv_command` |
| 267 | HIGH | Data Race on `preloaded_img` via Detached Threads — multiple threads writing to shared img | Used `local_img` and assigned to `preloaded_img` under `preload_mutex` |
| 268 | MEDIUM | State Inconsistency after Playlist Hot-Swap — `next_index` and `loaded_tex` not reset | Reset `next_index` and unloaded `loaded_tex` during hot-swap |
| 269 | HIGH | `MPVPlayer::update_frame` leaks bound FBO on error — `glBindFramebuffer(0)` skipped | Ensured `glBindFramebuffer(0)` and `rlViewport` reset on error |
| 270 | MEDIUM | `MPVPlayer::play` incorrect escaping in array form — escapes quotes for `mpv_command` array | Removed manual shell escaping for array-form commands |
| 271 | MEDIUM | `Slideshow::update` potentially using `current_index` OOB before `advance()` is called | Clamped `current_index` at start of `update()` |

## Bug Fix Round 23 (272-281)

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| 272 | HIGH | Undeclared Variable `cmd` in `MPVPlayer::play` | Fixed by declaring `args` array and passing it to `mpv_command` |
| 273 | HIGH | Broken Timeout Logic in Root Scan — `join()` blocks timeout loop | Replaced `std::thread` with `std::async` and `wait_for` timeout |
| 274 | MEDIUM | Dangerous Use of `pthread_cancel` on `std::thread` | Removed `pthread_cancel` in favor of `std::async` timeout |
| 275 | MEDIUM | Data Race on `MPVPlayer::current_file` string | Wrapped updates and clears of `current_file` in `play_mutex` |
| 276 | MEDIUM | Playlist Swap Inconsistency — `current_index` atomic vs `shuffle_mutex` | Captured `frame_current_index` and `frame_next_index` under `shuffle_mutex` in `update` and `render` |
| 277 | LOW | SQLite Handle Leak in `CacheManager::open` failure paths | Added `close()` calls and `db = nullptr` on all `open` failure paths |
| 278 | LOW | Heap Memory Leak (`CacheManager`) — `g_cache` never deleted | Verified `main` calls `delete g_cache` on exit (v16.7.0) |
| 279 | LOW | Unprotected `std::stoi` calls in `is_in_seasonal_window` | Wrapped `std::stoi` in try-catch block |
| 280 | LOW | Inefficient `Treadmill` midnight sync loop | Reduced sleep interval to 30s for better precision |
| 281 | LOW | Potential VRAM Misalignment in `LoadImageHEIC` (3-byte pixel format) | Converted RGB to RGBA (4-byte) for better alignment and GLES2 compatibility |
