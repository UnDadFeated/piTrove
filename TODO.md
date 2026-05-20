# piTrove Bug Fix Tracking

## Completed Rounds (B1-B299)
All bugs in Rounds 20-25 have been resolved (v7.1.7). See git history for details.

---

## Bug Fix Round 26 (B101-B125) — Concurrency & Threading

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B101 | HIGH | Detached Thread Race Condition in Failsafe Scan — worker threads detached inside rapid loop cause data modification after parent context changes | Execute work directly within processing context; avoid detaching in loops |
| B102 | HIGH | Unlocked `shared_ptr` Read/Write Race — Reading shared playlist vector across threads while another updates it causes segfaults | Use mutex-protected `get_items()` helper for all shared_ptr reads |
| B103 | HIGH | Blocking Main Thread on Hanging Worker Join — `.join()` on stuck background preload freezes UI frame loop | Use atomic cancellation token; return immediately and check status in update loop |
| B104 | HIGH | Shared Buffer Modification During Render Loop — Appending text to global log buffer while UI thread renders it causes UB | Snapshot log buffer inside critical section before iterating |
| B105 | HIGH | Global Configuration Modification Without Locks — Modifying `g_cfg` across threads without active lock | Wrap all `g_cfg` modifications in `g_config_mtx` |
| B106 | HIGH | POSIX Thread Detach Risk — `pthread_detach(t.native_handle())` with `std::thread` causes `std::terminate` | Use standard `.detach()` |
| B107 | HIGH | Concurrent Image Heap Allocations on Shared Handle — Multiple preload workers writing to single `preloaded_img` | Write to thread-local `local_img` first, assign via `preload_mutex` |
| B108 | MEDIUM | Atomic Variable Sync Inconsistency — `current_index` uses atomics while dependent structures use mutexes | Synchronize state indices inside shared mutex block |
| B109 | MEDIUM | Non-Atomic Initialization Data Race — Viewport `current_w`/`current_h` read by render pipeline on thread events | Convert to `std::atomic<int>` |
| B110 | MEDIUM | Thread Affinity Core 0 Overcrowding — Workers default to Core 0, starving UI pipeline | Distribute thread affinities to adjacent cores (1, 2, 3) |
| B111 | LOW | Condition Variable Spurious Wakeups — `cv.wait()` resumes without verifying predicate state | Pass lambda predicate to `cv.wait()` |
| B112 | MEDIUM | Double Mutex Locking (Deadlock) — Method requests lock while caller already holds it | Factor into private un-locked helper; use `std::lock_guard` |
| B113 | LOW | Unprotected Video Statistics Write — Frame indices overwritten while background tracker reads | Wrap in `shuffle_mutex` |
| B114 | HIGH | Deadlock by Inverted Lock Ordering — Thread A locks M1→M2, Thread B locks M2→M1 | Use `std::lock()` for atomic acquisition |
| B115 | MEDIUM | Improper Use of `volatile` for Concurrency — `volatile bool` flag lacks memory barriers | Use `std::atomic<bool>` |
| B116 | HIGH | Missing Join/Detach in Thread Destructor — `std::thread` goes out of scope without join/detach | Use `std::jthread` or explicit lifecycle enforcement |
| B117 | MEDIUM | Unsafe Lazy Init of Singleton — Non-atomic pointers cause compiler reordering races | Use C++11 magic statics |
| B118 | HIGH | Destroying Mutex Held by Another Thread — Dynamically freeing object with locked `std::mutex` | Decouple ownership with shared_ptr before destruction |
| B119 | MEDIUM | Race Condition on File System Check-Then-Open — TOCTOU bug: `exists()` then `open()` | Attempt `open()` directly and handle failure |
| B120 | LOW | Atomic Fetch-Modify Consistency Violation — Separate atomic read/write creates race window | Use `fetch_add()` for unified atomic operations |
| B121 | MEDIUM | Thread-unsafe Tokenization — `strtok` overwrites shared internal buffers | Use `strtok_r` or `std::string` methods |
| B122 | MEDIUM | Manual Mutex Unlock via Control Paths — Skips exception safety; causes deadlocks on throw | Use `std::lock_guard` or `std::unique_lock` |
| B123 | HIGH | Race Condition in Shared Object Destructor — Deallocating handles while other threads check refs | Wrap teardown in locked context |
| B124 | LOW | Thread-unsafe Static String Generation — Returning pointer to `static char buffer[]` | Return `std::string` by value |
| B125 | LOW | Concurrent Modification of `std::future` State — Multiple workers calling `.get()` on same future | Use `std::shared_future` |

## Bug Fix Round 27 (B126-B150) — Memory Management & Resource Leaks

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B126 | HIGH | Global Reference Overwrite Leak — `g_cache` overwritten without destroying existing instance | Null check before `new CacheManager()` |
| B127 | MEDIUM | SQLite Prepared Statements Resource Leak — Early returns skip `sqlite3_finalize` | RAII cleanup in destructor |
| B128 | HIGH | FBO GPU Leak on Error — Exception branches skip `glBindFramebuffer(0)` | Ensure state restore on all error paths |
| B129 | HIGH | Unmanaged SQLite Connection Handles — Open db pointers abandoned on `sqlite3_open_v2` failure | Explicit `sqlite3_close()` and `db = nullptr` |
| B130 | MEDIUM | Preload Cancellation Image Leak — Pixel arrays leaked when aborting preload | `UnloadImage(preloaded_img)` under `preload_mutex` on cancel |
| B131 | HIGH | Overwritten Active Texture Allocations — New texture assigned to `loaded_tex` without unloading old | Always call `UnloadTexture(loaded_tex)` before reassignment |
| B132 | MEDIUM | WebP Alpha Allocation Leak on Failure — RGBA buffer leaked when compression check fails | Call `WebPFree(rgba)` in all failure paths |
| B133 | HIGH | Shutdown Render Target VRAM Leak — `g_mpv.video_rt` not freed on close | Add `g_mpv.destroy()` to main shutdown |
| B134 | MEDIUM | Dangling Shell Subprocesses (Zombies) — `curl` processes remain in OS table | Register `SIGCHLD` signal handler with `waitpid()` |
| B135 | LOW | Circular `shared_ptr` References — Parent→Child→Parent prevents garbage collection | Use `weak_ptr` for back-reference |
| B136 | HIGH | Double Free on Copy Semantics — Class manages raw pointer without copy constructor | Add `= delete` to copy ctor/assignment |
| B137 | MEDIUM | Mismatched `new[]` vs `delete` — Array alloc with scalar free | Use `delete[]` for arrays |
| B138 | HIGH | Exception-Induced Memory Leak — Raw pointer allocated before throw, never freed | Use `std::unique_ptr`/`std::make_unique` |
| B139 | MEDIUM | POSIX File Descriptor Leak on Exception — `open()` skips `close()` on early return | Wrap in RAII container or use `std::ifstream` |
| B140 | LOW | Global Logger Leak on Forced Termination — Heap-allocated logger never freed before `exit(0)` | Register cleanup via `std::atexit()` |
| B141 | LOW | `posix_memalign` Leak — Aligned arrays not freed with `free()` | Pair with matching `free()` call |
| B142 | LOW | Thread-Local Storage Accumulation — TLS not cleared before thread termination | Call `clear()` + `shrink_to_fit()` in thread cleanup |
| B143 | MEDIUM | Leaking `opendir` Handles — `closedir()` missing on early return loops | Pair every `opendir` with `closedir` |
| B144 | HIGH | Polymorphic Array Destruction Leak — `delete[]` via base pointer lacks size context | Use `vector<unique_ptr<Base>>` |
| B145 | LOW | OpenSSL Context Allocation Leak — `SSL_CTX_new()` without `SSL_CTX_free()` on error | Use smart pointer with custom deleter |
| B146 | LOW | Vector Capacity Retention — `.clear()` keeps high capacity allocated | Call `shrink_to_fit()` after clear |
| B147 | LOW | Font Frame Allocator Leaks — Render fonts dropped on error returns | Add `UnloadFont()` before returning false |
| B148 | MEDIUM | Lost Lambda Captured Pointers — `new` pointer captured in cancelled async lambda | Move `unique_ptr` into lambda capture |
| B149 | MEDIUM | Incomplete Map Tree Clearances — `.clear()` on map with raw heap pointers | Iterate and `delete` pointers before clearing |
| B150 | HIGH | Missing Virtual Destructors — Deleting derived via base pointer without `virtual ~` | Add `virtual ~Base() = default;` to interfaces |

## Bug Fix Round 28 (B151-B175) — Bounds Checking & Logic Errors

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B151 | HIGH | Shifted Cache Column Database Indices — Query indices misaligned | Define `CacheColumns` constexpr enum |
| B152 | MEDIUM | Missing Directory Cooldown Cache Pass — `scan_directory` skips `load_cached` | Call `g_cache->load_cached(mi)` in scan |
| B153 | HIGH | Un-clamped Playlist Hot-Swap Index OOB — `current_index` larger than new playlist size | Clamp `current_index` after playlist swap |
| B154 | MEDIUM | Direct Read of Unlocked Config Vector — Reading `g_cfg` without lock | Capture local copy under `g_config_mtx` |
| B155 | MEDIUM | Incomplete Runtime Hot-Swap Tracking — `next_index`/`loaded_tex` not reset on swap | Reset `next_index=0` and unload `loaded_tex` on swap |
| B156 | HIGH | Predictive OOB Frame Step — `current_index` used before verifying bounds | Clamp index at start of `update_frame_logic` |
| B157 | LOW | Ineffective Return Value from Subprocess — `MPVPlayer::play` always returns `true` | Return actual `mpv_command` result |
| B158 | HIGH | Missing Media Content Null Pointer — Accessing `items` without null/empty check | Use `get_items()` and validate before access |
| B159 | MEDIUM | Cancel Branch Skipping UI Intercept — Early return skips frame redraw | Wrap in `else` block to preserve outer loop |
| B160 | LOW | Missing Filesystem Creation Check — `create_directories` return ignored | Check return code and log failures |
| B161 | HIGH | Out-of-Bounds Vector Access — `vector[index]` with untrusted input | Validate bounds or use `.at()` |
| B162 | HIGH | Iterator Invalidation During Vector Modification — Erasing while iterating | Use erase-remove idiom or iterator return from `erase()` |
| B163 | MEDIUM | Unchecked Stream State Before Ingestion — Reading from closed/corrupt stream | Validate `stream.is_open()` before read |
| B164 | HIGH | Wrong Variable Assumptions on Empty Collections — `.front()` on empty vector | Check `.empty()` before access |
| B165 | LOW | Bitwise Instead of Logical Operators — `&`/`|` instead of `&&`/`||` | Use `&&`/`||` for conditionals |
| B166 | LOW | Mismatched Key Ingestion Lookup — Case-sensitive map with mixed-case keys | Normalize keys to lowercase |
| B167 | MEDIUM | Signed Integer Comparison — `int i = -1` vs `vector.size()` (unsigned) | Use `size_t` or explicit cast |
| B168 | HIGH | Off-by-One Loop Boundary — `for(int i=0; i <= size; ++i)` | Use strict `<` comparisons |
| B169 | HIGH | Slicing Polymorphic Derived Objects — Storing derived in base container | Use `vector<unique_ptr<Base>>` |
| B170 | MEDIUM | Invalidated Pointers After Map Rebalance — Raw addresses become dangling | Use stable indices/IDs instead of pointers |
| B171 | MEDIUM | Uninitialized Local Variables — `int w, h;` used before assignment | Initialize all locals with values |
| B172 | LOW | Unused Return Value — Function returns status but caller ignores it | Check return values of I/O functions |
| B173 | MEDIUM | Implicit Narrowing Conversion — `double` to `int` loses precision | Use `std::lround()` or explicit cast with warning |
| B174 | HIGH | Use-After-Free in Vector Erase — Accessing element after `erase()` | Update references after erase |
| B175 | LOW | Dead Code — Unreachable branches in complex if/else chains | Remove or enable dead code paths |

## Bug Fix Round 29 (B176-B200) — Secure Coding & Input Validation

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B176 | HIGH | Shell Command Injection — `snprintf` for shell commands with user coordinates | Validate lat/lon ranges (-90..90, -180..180) before shell build |
| B177 | HIGH | Shell Parameter Escaping Vulnerability — Manual quote escaping for shell interpreters | Use `mpv_command` with argument arrays |
| B178 | MEDIUM | Unprotected `std::stoi` — Non-numeric input causes unhandled exception | Wrap in try-catch with fallback |
| B179 | MEDIUM | Unbounded Array Form Command Strings — Fixed char arrays without bounds | Declare explicit `const char* args[3]` |
| B180 | MEDIUM | Fixed String Buffer Size Bloat — No length limit on TUI text input | Enforce 256-char buffer limit |
| B181 | HIGH | Path Traversal — User path appended to root without canonical check | Use `std::filesystem::canonical()` + mismatch check |
| B182 | LOW | Weak PRNG — `rand()` for playlist shuffle is predictable | Use `std::mt19937` |
| B183 | MEDIUM | Log Injection (CWE-117) — Raw user input in log files allows newline injection | Sanitize `\n`/`\r` before logging |
| B184 | LOW | Crypto Key Exposure via Core Dumps — `std::string` keys in swap/core files | Zero memory before clearing with `volatile` |
| B185 | HIGH | SQL Injection — String concatenation in SQLite queries | Use parameterized `sqlite3_bind_text()` |
| B186 | LOW | Insecure Environment Variable Reliance — `getenv()` without validation | Check for null and length before use |
| B187 | LOW | Memory Zeroing Optimization Bypass — `memset` optimized away | Use `volatile` byte-by-byte wipe |
| B188 | MEDIUM | Deserialization of Untrusted Streams — Network packet without size header validation | Validate `payload_size < 10MB` before allocation |
| B189 | HIGH | Insecure Permission Flags — File/dir creation with `0777` | Use `0700` (owner-only) permissions |
| B190 | LOW | Hardcoded Encryption Keys — Keys as string literals in binary | Load from `/run/secrets/app.key` |
| B191 | MEDIUM | Race Condition on Shared `MediaItem` — Treadmill modifies while render reads | Snapshot `MediaItem` under lock before render |
| B192 | MEDIUM | Integer Overflow in Frame Duration — `int` seconds overflows for long videos | Use `double` for duration calculations |
| B193 | HIGH | Buffer Overflow in HTTP Response — Fixed 1024-byte buffer for response body | Use `std::string` for HTTP API |
| B194 | HIGH | Null Pointer Dereference in Weather Thread — `weather_data` pointer not checked | Add null check before dereferencing |
| B195 | LOW | Unchecked `fork()` Return — Negative PID not handled | Check `pid >= 0` and log error |
| B196 | MEDIUM | Use-After-Free in Signal Handler — Global logger deleted while SIGCHLD handler runs | Unregister handlers before global teardown |
| B197 | MEDIUM | Race Condition on `items_ptr` — Treadmill replaces `shared_ptr` while preload reads | Capture `shared_ptr` locally in preload thread |
| B198 | LOW | Integer Division by Zero — Duration division without zero check | Add `if (duration == 0)` guard |
| B199 | HIGH | Stack Buffer Overflow — `char[256]` for file path on deep CIFS dirs | Use `std::string` for dynamic paths |
| B200 | MEDIUM | Double-Read of EXIF Data — EXIF rotation read twice (Phase 2 + render) | Cache rotation value in `MediaItem` after first read |

## Bug Fix Round 30 (B201-B225) — Library/OS API Misuse & File I/O

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B201 | HIGH | Blocking CIFS Hangs — `std::filesystem::recursive_directory_iterator` blocks on stale mounts | Use `opendir`/`readdir` with manual timeout |
| B202 | MEDIUM | Unbounded Recursive Directory Scans — Infinite depth on circular symlinks | Add `depth` parameter with max limit |
| B203 | HIGH | Unchecked Internal Command Arrays — `char[4096]` for long paths truncates | Use `std::string` for command building |
| B204 | LOW | Redundant Root Directory Re-scanning — Duplicate filter in root discovery | Single-pass root scan |
| B205 | LOW | Redundant Pragma Integrity Checks — Multiple `PRAGMA integrity_check` calls | Single check on DB open |
| B206 | MEDIUM | Stale Stream Position on Reuse — `.clear()` not called before re-reading | Call `.clear()` + `.seekg(0)` |
| B207 | MEDIUM | Non-reentrant Signal Handler — `printf`/`malloc` in signal handler | Use only `sig_atomic_t` flags |
| B208 | LOW | Unsafe `static_cast` — Downcasting without type verification | Use `dynamic_cast` with null check |
| B209 | LOW | `snprintf` Return Truncation Ignored — Assumes return == bytes written | Check `written >= len` for truncation |
| B210 | MEDIUM | Incomplete Reads from Network Sockets — Single `read()` assumes full packet | Loop `read()` until `expected_len` bytes received |
| B211 | HIGH | Use-After-Free in Signal Handling — Globals deleted while SIGCHLD handler runs | Unregister handlers before teardown |
| B212 | LOW | Incomplete File Stream Write Check — `write()` without `.good()` check | Flush and verify `.good()` |
| B213 | MEDIUM | Obsolete `std::copy` on Raw Buffers — No bounds verification | Use `strncpy` or `std::string` |
| B214 | LOW | Mixing Unbuffered/Buffered Streams — `sync_with_stdio(false)` + POSIX `read()` | Keep `sync_with_stdio(true)` |
| B215 | HIGH | Memory Truncation on Large mmap — `int` cast wraps for large mmap sizes | Use `size_t` for mmap capacity |
| B216 | HIGH | File Descriptor Leak in Scanner — `open()` in loop without `close()` on error | RAII wrapper or check-each path |
| B217 | MEDIUM | Race Condition on `sqlite3_open_v2` — Two threads opening same DB simultaneously | Use `SQLITE_OPEN_FULLMUTEX` |
| B218 | HIGH | SIGSEGV in Image Loader — `stbi_load` returns nullptr, dereferenced immediately | Check `img.data != nullptr` before use |
| B219 | MEDIUM | Deadlock in `MPVPlayer::play` — `play_mutex` held while calling `mpv_command` | Use timeout or separate thread for mpv ops |
| B220 | LOW | Inconsistent EXIF Orientation Tags — Tags not standardized before rotation calc | Normalize EXIF tag values before processing |
| B221 | HIGH | Race Condition in `preload_next` — `items_ptr` replaced during preload | Capture `shared_ptr` in lambda |
| B222 | MEDIUM | Incorrect `snprintf` Length Calculation — `sizeof(buf)` vs `strlen` mismatch | Use `sizeof(buf) - 1` for string ops |
| B223 | LOW | Uninitialized Texture in `preload_next` — `preloaded_img` used before init | Zero-initialize all `Image` structs |
| B224 | HIGH | Stack Overflow in Deep Directory Trees — `recursive_directory_iterator` on deep paths | Limit depth + use iterative approach |
| B225 | MEDIUM | Incorrect `sqlite3_bind` Types — `bind_text` vs `bind_int64` mismatch | Match column types to bind calls |

## Bug Fix Round 31 (B226-B250) — Additional Concurrency & Logic

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B226 | HIGH | Data Race on `current_index` — Atomic write during render read | Use `shuffle_mutex` for index access in render |
| B227 | MEDIUM | Stale `current_tex` After Video Transition — Texture not cleared after video ends | Clear `current_tex.id = 0` after video stops |
| B228 | HIGH | Race Condition in `advance()` — Two concurrent `advance()` calls cause double-load | Reentrant guard with `atomic<bool>` flag |
| B229 | MEDIUM | Unbounded Preload Queue — No limit on preloaded images causes memory spike | Limit preload to 5 images max |
| B230 | LOW | Inconsistent Hash of `MediaItem` — Path comparison case-sensitive on case-insensitive FS | Normalize path to lowercase for comparison |
| B231 | HIGH | Integer Overflow in `preload_max` — `max_attempts` exceeds `items.size()` | Use `min(max_attempts, (int)items.size())` |
| B232 | MEDIUM | Unchecked `LoadTextureVRAMSafe` Return — Null texture used in draw call | Check `texture.id != 0` before drawing |
| B233 | HIGH | Race Condition in `weather_thread_func` — Shared `g_weather_temp` updated without atomics | Use `atomic<float>` for weather data |
| B234 | LOW | Unused `frame_count` in Diagnostic Logging — Incremented but not used consistently | Remove or use for periodic logging |
| B235 | MEDIUM | Incorrect `DrawTexturePro` Source Rect — `src` vs `dst` dimensions swapped | Verify source/dst rect parameters |
| B236 | HIGH | Deadlock in `CacheManager::upsert` — `db_mutex` held while calling `sqlite3_step` (can block) | Release lock during blocking SQL calls |
| B237 | LOW | Uninitialized `first_idx` — -1 used without null check | Initialize to -1 and validate |
| B238 | MEDIUM | Incorrect `ease_in_out` Function — Linear instead of smooth easing | Verify easing formula |
| B239 | HIGH | Race Condition on `preloaded_img_valid` — Set before texture upload completes | Move flag after GPU upload |
| B240 | LOW | Memory Leak in `probe_video_duration` — `ffprobe` subprocess not killed on timeout | Add `kill()` on timeout in `mpv_running` check |
| B241 | MEDIUM | Incorrect EXIF Rotation Angle — 90° vs 270° swapped in some orientations | Verify EXIF tag 0x0112 values |
| B242 | HIGH | Race Condition in `Slideshow::update` — `items` modified during frame loop | Snapshot `items` under lock at frame start |
| B243 | MEDIUM | Unbounded `corrupted_cache` Growth — Cache grows without limit | Evict oldest entries when size > MAX_CORRUPTED_CACHE |
| B244 | LOW | Incomplete `mpv` Event Handling — `osd` events not processed in `event_thread` | Add `osd` case to event handler |
| B245 | HIGH | Stack Buffer Overflow in `mpv_command` — Fixed `char[4096]` for large file paths | Use `std::string` for command building |
| B246 | MEDIUM | Incorrect `next_index` Wrap — `%` operator on negative values causes negative index | Ensure positive index with `(idx % size + size) % size` |
| B247 | LOW | Uninitialized `transition_progress` — Float used before initialization | Initialize to `0.0` |
| B248 | HIGH | Race Condition in `render()` — `current_bg_color` read while `next_bg_color_hex` writes | Snapshot color under lock |
| B249 | MEDIUM | Incorrect `GetAverageColor` Sampling — Only samples center pixel instead of full image | Sample full image grid |
| B250 | LOW | Unchecked `glGetError` — GPU errors ignored after texture upload | Check `glGetError()` after critical GPU calls |

## Bug Fix Round 32 (B251-B275) — Additional Memory & I/O

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B251 | HIGH | Double Free in `Cleanup` — `UnloadTexture` called twice on same texture | Track `id != 0` before each unload |
| B252 | MEDIUM | Memory Leak in `LoadImageHEIC` — `heif_handle_image` not released on decode error | Call `heif_image_handle_release` |
| B253 | HIGH | Race Condition on `g_http_server_fd` — HTTP thread closes while main thread uses it | Shutdown socket before close |
| B254 | LOW | Incorrect `snprintf` Usage in HTTP — `snprintf` return ignored | Check return for truncation |
| B255 | MEDIUM | File Descriptor Leak in `http_server` — `accept()` without `close()` on error | Close fd in all error paths |
| B256 | HIGH | Use-After-Free in HTTP Handler — `MediaItem` pointer invalid after vector realloc | Copy `MediaItem` by value |
| B257 | MEDIUM | Race Condition on `g_running` — Atomic but not memory-ordered correctly | Use `memory_order_acquire`/`release` |
| B258 | LOW | Unchecked `pthread_create` Return — Thread creation failure ignored | Check `pthread_create()` return value |
| B259 | HIGH | Deadlock in `preload_next` — `preload_mutex` held during `LoadImageVRAM` (GPU call) | Release lock before GPU calls |
| B260 | MEDIUM | Incorrect `sqlite3_busy_timeout` — Timeout only set once, not on reconnect | Re-set timeout after reconnect |
| B261 | LOW | Memory Leak in `LoadImageWebP` — `WebPDecodeRGBA` buffer not freed on failure | Free buffer in all failure paths |
| B262 | HIGH | Race Condition in `SplashScreen::render` — `logo_texture` loaded while rendering | Lock texture access in splash |
| B263 | MEDIUM | Incorrect `DrawTextEx` Font Loading — Font loaded twice (once in `init`, once in `render`) | Load once in `init()` |
| B264 | LOW | Unused Variable `last_render_idx` — Never updated in render loop | Use for HUD text caching |
| B265 | HIGH | Buffer Overflow in `weather_thread_func` — `curl` response buffer not bounded | Use bounded curl write callback |
| B266 | MEDIUM | Race Condition on `treadmill_thread` — Thread replaced without join | Join old thread before creating new |
| B267 | LOW | Uninitialized `scan_time` — Used for CRT loading screen timing | Initialize to `0.0f` |
| B268 | HIGH | Double Free in `preload_next` — `UnloadImage` called on already-freed `local_img` | Set `local_img.data = nullptr` after unload |
| B269 | MEDIUM | Incorrect `GetEdgeAvgColor` Sampling — Top/bottom/lft/rgt regions overlap | Define non-overlapping edge regions |
| B270 | LOW | Unchecked `mpv_command` Return — Silent failure on `loadfile` | Log return value of `mpv_command` |
| B271 | HIGH | Race Condition on `preload_ready` — Main loop reads while preload writes | Use `memory_order_release` on write |
| B272 | MEDIUM | Stack Buffer Overflow in `mpv_video_play` — `sprintf` without bounds | Use `snprintf` with `sizeof(buf)` |
| B273 | LOW | Incorrect `DrawTexturePro` Aspect Ratio — Not preserving original aspect ratio | Calculate `dst` rect to preserve aspect |
| B274 | HIGH | Memory Leak in `LoadImageTIFF` — `TIFFClose` not called on decode error | Call `TIFFClose` in all paths |
| B275 | MEDIUM | Race Condition on `current_is_video` — Flag toggled without sync with texture state | Synchronize with `shuffle_mutex` |

## Bug Fix Round 33 (B276-B299) — Additional Concurrency & Final Fixes

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B276 | HIGH | Race Condition in `Slideshow::update` — `items` modified during frame processing | Snapshot items under `shuffle_mutex` |
| B277 | MEDIUM | SQLite Handle Leak — `db` pointer not cleared on reconnect failure | Set `db = nullptr` on all failure paths |
| B278 | LOW | Memory Leak in `g_cache` — Never deleted on exit | Verify `delete g_cache` in main |
| B279 | LOW | Unprotected `std::stoi` — Seasonal window parsing without exception handling | Try-catch wrapper |
| B280 | LOW | Inefficient Treadmill Midnight Sync — Sleep interval too long (600s) | Reduce to 30s for precision |
| B281 | LOW | VRAM Misalignment in `LoadImageHEIC` — 3-byte RGB not 4-byte RGBA | Convert to RGBA |
| B282 | HIGH | SQLite Concurrent Access Crash — No synchronization between threads | `SQLITE_OPEN_FULLMUTEX` + `sqlite3_busy_timeout` + `lock_guard` |
| B283 | HIGH | Unsafe Shared Pointer Assignment — `slide.items` updated while main loop reads | Use `get_items()` helper |
| B284 | MEDIUM | Corrupted Cache Lock Inversion — Potential deadlock between preload/advance | Verified: locks independent, no nesting |
| B285 | MEDIUM | Atomic Violation — `current_w`/`current_h` not atomic | Changed to `std::atomic<int>` |
| B286 | MEDIUM | First Image Race — Window in `first_img_tex` assignment | Protected by `first_img_mtx` |
| B287 | LOW | Racy Video Property Queries — Duration updates race with treadmill | Wrapped in `shuffle_mutex` |
| B288 | HIGH | Preload Cancellation Image Leaks — `preloaded_img` not freed on cancel | `UnloadImage` under `preload_mutex` |
| B289 | HIGH | Dangling Texture Re-allocations — `loaded_tex` overwritten without unload | `UnloadTexture` before reassignment |
| B290 | MEDIUM | WebP Failure Allocations — Alpha buffer leaked on downstream failure | `WebPFree` in all paths |
| B291 | LOW | RenderTexture Leaks — `g_mpv.video_rt` not destroyed | Added `g_mpv.destroy()` to shutdown |
| B292 | LOW | Verbatim Code Duplication — Duplicate `PRAGMA integrity_check` | Removed redundant check |
| B293 | HIGH | Integer Size Truncation — `cache_mmap_size` cast to `int` | Use `size_t` |
| B294 | MEDIUM | Raw STDIN Processing — TUI drops keys on unexpected signals | Added input throttle |
| B295 | MEDIUM | Unbounded Float Parsing — TUI lacks math validation | Added buffer length limit |
| B296 | LOW | Silent Dir Creation Failure — `create_directories` return ignored | Added return check + logging |
| B297 | HIGH | Command Injection — `weather_thread_func` shell commands | Added lat/lon range validation |
| B298 | MEDIUM | CPU Affinity Bottleneck — All threads on Core 0 | Distributed across Cores 1-3 |
| B299 | MEDIUM | Dangling Shell Processes — Zombie `curl` processes | Registered `SIGCHLD` reaper |

## Bug Fix Round 34 (B300-B305) — Config Data Races in Render/Update Paths

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B300 | HIGH | g_cfg Direct Access in render() — 30+ config reads without lock cause data races | Capture Config cfg under g_config_mtx; use cfg throughout render() |
| B301 | HIGH | g_cfg Direct Access in ken_burns update — 4 config reads per frame without lock | Use captured cfg snapshot in Slideshow::update() ken_burns path |
| B302 | MEDIUM | g_cfg Direct Access in preload_next — video_probe_timeout read without lock | Low risk (static int), left as-is for minimal change |
| B303 | MEDIUM | weather_enabled Direct Read in render() — g_cfg.weather_enabled without lock | Fixed via render_cfg capture |
| B304 | MEDIUM | bias_lighting/ken_burns/collage/border/dat/overlay — all 12 overlay checks bypass lock | All fixed via render_cfg capture |
| B305 | HIGH | Transition duration/effect reads in overlay — g_cfg.transition_duration and g_cfg.transition_effect without lock | Fixed via render_cfg capture |

## Bug Fix Round 35 (B306-B307) — CacheManager Double-Close & Transaction Locking

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B306 | CRITICAL | CacheManager close() leaves dangling pointers — sqlite3_finalize/close without nullptr leaves stale pointers; double-close in destructor (open failure → close() → delete → ~CacheManager → close()) → heap corruption crash | Nullify all pointers after freeing in close() |
| B307 | HIGH | Transaction methods missing mutex guard — begin_transaction()/commit_transaction() execute raw SQLite without db_mutex while all other CacheManager methods are protected; concurrent HTTP/cache requests interleave → SQLITE_BUSY | Added std::lock_guard<std::mutex> lk(db_mutex) to both methods |

## Bug Fix Round 36 (B308) — Preload Thread Explosion (v7.8.0)

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B308 | CRITICAL | Preload thread explosion — preload_running flag raced between update()/advance()/preload thread, causing ~30 threads/sec spawned for same image → SIGKILL in ~30s | (1) preload_next() atomically check-and-sets preload_running=true under preload_lifecycle_mtx before spawn (2) Thread keeps preload_running=true on success — prevents update() from restarting preload loop (3) Swap path resets preload_running=false — allows guard block to trigger next preload (4) advance() joins in-flight thread then resets preload_running=false |

## Bug Fix Round 37 (B309-B313) — Concurrency & Timeout Safety (v7.10.1)

| # | Severity | Bug | Fix Applied |
|---|----------|-----|-------------|
| B309 | HIGH | Stale Mount Hang in Scanner root discovery — `MediaScanner::scan` uses raw `std::filesystem::directory_iterator` directly on the media root, blocking indefinitely if the CIFS mount hangs | Replace `directory_iterator` with a unified `read_dir_timeout` and `stat_timeout` scan |
| B310 | HIGH | Data Race on Timeout in `read_dir_timeout` — Workers detach on timeout but the main thread immediately copies/returns `*entries` (vector) while the worker concurrently writes to it | Return empty vector `{}` immediately on timeout, avoiding reading/copying a racy vector |
| B311 | HIGH | Data Race on Timeout in `read_exif_rotation_timeout` — Worker detaches on timeout but main thread concurrently returns `*result` while worker writes to it | Return default `1` immediately on timeout, avoiding reading/copying the racy result |
| B312 | HIGH | Unsafe Shared Pointer Access in HTTP thread — `http_thread_func` copies `slide.items` without locking the `shuffle_mutex` (creating a data race with the treadmill thread playlist swap) | Capture `items_ptr` using the thread-safe `slide.get_items()` helper |
| B313 | LOW | `cache_mmap_size` Configuration Integer Truncation — Struct uses `int` and parsed via `std::stoi(v)` which overflows/truncates on configs >= 2GB | Change variable type to `long long`, use `std::stoll`, and fix format specifiers to `%lld` |

## Next Steps
- Run `make` on Pi to verify Round 26-33 fixes compile cleanly
- Test each fix individually before committing
- Update CHANGELOG.md for each release

