# TODO - piTrove Active Bug Backlog

This file tracks identified bugs, security vulnerabilities, memory leaks, and concurrency/race conditions across the piTrove codebase. All items have been audited for safety to ensure that implementing their fixes will not break other application subsystems.

---

## Active Backlog (17 Items)

### 1. Undefined Behavior: `pclose(nullptr)` on `popen` Failure
*   **Severity**: Critical
*   **Affected Files**:
    *   [google_photos.cpp](file:///P:/piTrove/src/google_photos.cpp#L332-L334)
    *   [http_server.cpp](file:///P:/piTrove/src/http_server.cpp#L86-L87)
    *   [preprocess.cpp](file:///P:/piTrove/src/preprocess.cpp#L43-L44)
*   **Description**: In multiple utility execution wrappers, `std::shared_ptr<FILE> pipe(popen(...), pclose)` is constructed before checking the return value. If `popen` fails and returns `nullptr`, the shared pointer's destructor will invoke `pclose(nullptr)`, which causes undefined behavior / segmentation faults.
*   **Proposed Fix**: Assign the result of `popen` to a raw pointer, verify it is not `nullptr`, and only construct the `std::shared_ptr` if validation passes.

### 2. Async-Signal-Unsafe Heap Allocation in MQTT Fork
*   **Severity**: High
*   **Affected Files**:
    *   [mqtt.cpp](file:///P:/piTrove/src/mqtt.cpp#L250-L283)
*   **Description**: Between `fork()` and `execvp()`, the child process constructs a `std::vector<char*> argv` and performs string conversions. Because another thread may hold the memory allocator (malloc/free) lock during `fork()`, performing heap allocations in the child process can cause deadlocks.
*   **Proposed Fix**: Construct the `args` array and the `char*` vector in the parent process scope before calling `fork()`, ensuring the child process only invokes async-signal-safe system calls.

### 3. Async-Signal-Unsafe Heap Allocation in MPV Player Fork
*   **Severity**: High
*   **Affected Files**:
    *   [mpv_player.cpp](file:///P:/piTrove/src/mpv_player.cpp#L171-L250)
*   **Description**: Similar to the MQTT subscriber process, the child process spawned to launch `mpv` performs extensive heap allocations (`std::vector<std::string>`, `std::to_string`, and `std::vector<char*>`) after `fork()`.
*   **Proposed Fix**: Build the full arguments and `char*` array in the parent thread scope prior to calling `fork()`.

### 4. Double-Destroy of Window Handle on Renderer Initialization Failure
*   **Severity**: Medium
*   **Affected Files**:
    *   [renderer.cpp](file:///P:/piTrove/src/renderer.cpp#L169-L175)
*   **Description**: In `Renderer::init()`, if `SDL_CreateRenderer()` fails, `SDL_DestroyWindow(window)` is called, but the member variable `window` is not reset to `nullptr`. When `cleanup()` is later executed by the destructor, it checks `if (window)` and calls `SDL_DestroyWindow` a second time.
*   **Proposed Fix**: Set `window = nullptr;` immediately after destroying it in the initialization failure path.

### 5. Over-broad Error Clearing Masking Unrelated Failures
*   **Severity**: Medium
*   **Affected Files**:
    *   [util.cpp](file:///P:/piTrove/src/util.cpp#L858-L861)
*   **Description**: In `check_network_status()`, if a previously detected network error (E102/E103) is cleared, the system calls `trigger_error(0)`. This purges *all* currently active errors in `g_active_errors`, masking unrelated issues such as SQLite disk errors or Google Photos sync states.
*   **Proposed Fix**: Replace `trigger_error(0)` with explicit calls to `clear_error(102)` and `clear_error(103)`.

### 6. Main Thread Stalls on Synchronous Prefetch Thread Join
*   **Severity**: High
*   **Affected Files**:
    *   [util.cpp](file:///P:/piTrove/src/util.cpp#L868-L875)
*   **Description**: The `prefetch_video` function checks `if (g_prefetch_thread.joinable())` and calls `g_prefetch_thread.join()` synchronously on the main thread. If the background thread is blocked on a slow or unresponsive network share, the slideshow will freeze.
*   **Proposed Fix**: Detach the previous `g_prefetch_thread` if it is still running (`g_prefetch_thread.detach()`) before overwriting it with the new thread.

### 7. TOML Parser Preserves Trailing Comments in Values
*   **Severity**: Medium
*   **Affected Files**:
    *   [config.cpp](file:///P:/piTrove/src/config.cpp#L20-L29)
*   **Description**: The TOML configuration parser only ignores lines starting with `#` or `;`. If a config line contains a trailing comment (e.g., `drm_connector = "auto" # comment`), the parser splits on `=` and treats the comment as part of the string or boolean value.
*   **Proposed Fix**: Implement a helper function `strip_comments()` that scans the line and removes characters after `#` or `;` unless they reside inside a quoted string literal.

### 8. Hardcoded Save Paths in Settings Modules
*   **Severity**: Low
*   **Affected Files**:
    *   [http_server.cpp](file:///P:/piTrove/src/http_server.cpp#L1915)
    *   [overlay.cpp](file:///P:/piTrove/src/overlay.cpp#L1036)
*   **Description**: When updating configuration options via the web remote or direct display overlay menu, `g_cfg.save` is hardcoded to `/app/config/config.toml`. If the app was started with a custom config path (e.g. from the command-line), updates are saved to the wrong file.
*   **Proposed Fix**: Capture and store the loaded configuration file path inside the `Config` struct (e.g., `loaded_path`), and use it as the target for all settings save operations.

### 9. Unchecked `localtime_r` Return Values
*   **Severity**: Medium
*   **Affected Files**:
    *   [main.cpp](file:///P:/piTrove/src/main.cpp#L133-L134)
    *   [main.cpp](file:///P:/piTrove/src/main.cpp#L759-L761)
    *   [util.cpp](file:///P:/piTrove/src/util.cpp#L253)
    *   [util.cpp](file:///P:/piTrove/src/util.cpp#L315)
    *   [util.cpp](file:///P:/piTrove/src/util.cpp#L326)
*   **Description**: In multiple locations, `localtime_r` is called and its result is immediately dereferenced or passed to `strftime` without checking for a `nullptr` return. This risks application crashes if the system clock becomes invalid or system calls fail.
*   **Proposed Fix**: Verify `localtime_r` return values are not `nullptr` before dereferencing, falling back to a default `tm` structure or logging a warning.

### 10. Exception Risk: Unchecked `create_directories` in Google Photos Manager
*   **Severity**: Medium
*   **Affected Files**:
    *   [google_photos.cpp](file:///P:/piTrove/src/google_photos.cpp#L110)
*   **Description**: The call to `std::filesystem::create_directories(cache_dir)` on line 110 lacks an `error_code` parameter. If the cache directory resides on a read-only filesystem or encounters permission errors, the function throws an unhandled exception, causing the program to crash.
*   **Proposed Fix**: Remove the redundant call (as a secure `create_directories` with `error_code` is already executed on line 94) or add the error_code parameter.

### 11. Exception Risk: Unchecked `create_directories` in Cache Manager & Logger
*   **Severity**: Medium
*   **Affected Files**:
    *   [cache.cpp](file:///P:/piTrove/src/cache.cpp#L15)
    *   [util.cpp](file:///P:/piTrove/src/util.cpp#L309)
*   **Description**: The Logger and Cache Manager initialize directory structures using `std::filesystem::create_directories` without an `error_code` parameter. If the parent directories are read-only or unmounted, the application crashes immediately upon startup.
*   **Proposed Fix**: Supply a `std::error_code` object to both calls and check for errors.

### 12. Overly Restrictive Google Photos Domain Validation
*   **Severity**: Low
*   **Affected Files**:
    *   [google_photos.cpp](file:///P:/piTrove/src/google_photos.cpp#L283-L290)
*   **Description**: The sync loop enforces that media downloads only originate from the `googleusercontent.com` domain. However, Google Photos API also returns hostnames matching `*.ggpht.com`, causing valid media synchronization requests to be blocked as potential security risks.
*   **Proposed Fix**: Extend the whitelist to permit both `googleusercontent.com` and `ggpht.com` (along with their subdomains).

### 13. Data Race: Unprotected Playlist Initialization
*   **Severity**: High
*   **Affected Files**:
    *   [main.cpp](file:///P:/piTrove/src/main.cpp#L1739-L1845)
*   **Description**: During slideshow startup, the main thread reads and mutates `g_eligible` and `g_scanned_items` (erasing bad files, dynamically pairing twin portraits, and updating widths/heights) without locking `g_playlist_mtx`. Since the HTTP server and MQTT background threads are already active, this causes a potential data race.
*   **Proposed Fix**: Protect the playlist checks and modifications in the startup loop using `g_playlist_mtx`, unlocking it during heavy file I/O operations to avoid blocking other subsystems.

### 14. Missing Screen Size Bounds in Transition Renderers
*   **Severity**: Medium
*   **Affected Files**:
    *   [transition.cpp](file:///P:/piTrove/src/transition.cpp#L77)
    *   [transition.cpp](file:///P:/piTrove/src/transition.cpp#L99)
    *   [transition.cpp](file:///P:/piTrove/src/transition.cpp#L137)
*   **Description**: The `render_fade`, `render_wipe`, and `render_ken_burns` functions do not check if `screen_w` or `screen_h` are `<= 0` before performing layout math, potentially causing negative width/height clippings or target destinations in SDL.
*   **Proposed Fix**: Add a check `if (screen_w <= 0 || screen_h <= 0) return;` at the beginning of each transition rendering method.

### 15. Dubious ownership Git errors in `install.sh`
*   **Severity**: Medium
*   **Affected Files**:
    *   [install.sh](file:///P:/piTrove/install.sh#L673-L679)
*   **Description**: Because `install.sh` runs as root, checking out or pulling the Git repository (which is owned by the primary user) causes Git v2.35.2+ to abort with a "detected dubious ownership in repository" error, breaking updates.
*   **Proposed Fix**: Execute all git commands inside the repository directory using `sudo -u "$PRIMARY_USER" git`.

### 16. Async-Signal-Unsafe Formatted Output in Intercept Handlers
*   **Severity**: Medium
*   **Affected Files**:
    *   [util.cpp](file:///P:/piTrove/src/util.cpp#L119)
    *   [util.cpp](file:///P:/piTrove/src/util.cpp#L146)
*   **Description**: The crash and termination signal handlers (`crash_handler` and `terminate_handler`) call `snprintf` to format database cache filenames. `snprintf` is not async-signal-safe and can deadlock the application if the crash occurred while holding internal standard library formatting locks.
*   **Proposed Fix**: Replace the `snprintf` call with a simple, safe character copy loop to build the string.

### 17. Unresponsive Program Shutdown via Detached Media Scanner
*   **Severity**: High
*   **Affected Files**:
    *   [scanner.cpp](file:///P:/piTrove/src/scanner.cpp#L200)
    *   [scanner.cpp](file:///P:/piTrove/src/scanner.cpp#L231)
*   **Description**: When shutting down the digital picture frame, the watchman thread is detached if the scanner is currently executing a directory sweep. The scanning loops continue running in the background, risking crashes if they attempt to access global structures that are being destroyed by the main thread.
*   **Proposed Fix**: Check `g_running` inside the directory scanner loops and recursive calls to abort directory scans immediately upon application exit.
