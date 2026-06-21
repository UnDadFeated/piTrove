# TODO - piTrove Active Bug Backlog

This file tracks identified bugs, security vulnerabilities, memory leaks, and concurrency/race conditions across the piTrove codebase. All items have been audited for safety to ensure that implementing their fixes will not break other application subsystems.

---

## Active Backlog (10 Items)

### 1. Undefined Behavior: Unaligned Pointer Dereferencing on ARM Architectures
*   **Severity**: High
*   **Affected Files**:
    *   [renderer.cpp](file:///P:/piTrove/src/renderer.cpp#L110-L121)
*   **Description**: In `get_pixel_color()`, a raw pointer `uint8_t* p` is cast directly to `uint16_t*` and `uint32_t*` and dereferenced. Because raw surface pitches and channel strides are not guaranteed to align on 2-byte or 4-byte boundaries, direct dereferencing causes unaligned memory access. On the target ARM architecture (Raspberry Pi), this results in undefined behavior, silent performance degradation, or hardware alignment faults (bus errors).
*   **Proposed Fix**: Use `memcpy` to copy the pixel bytes into a properly aligned local variable before dereferencing, ensuring safe access across all CPU architectures.

### 2. Unhandled Exception: File Path Relative Logic in Archive Organizer
*   **Severity**: Medium
*   **Affected Files**:
    *   [organizer.cpp](file:///P:/piTrove/src/organizer.cpp#L92)
*   **Description**: In `organize_media_archive()`, `std::filesystem::relative(src_path, root_dir)` is called inside a loop to determine relative directory paths. If the mount point contains dead symlinks or invalid permissions, `relative()` throws a `filesystem_error`. Because there is only a single top-level try-catch block for the entire directory scan, any exception throws aborts the entire organization process instead of skipping the bad item.
*   **Proposed Fix**: Add a localized try-catch block or pass a `std::error_code` parameter to `std::filesystem::relative` to skip the failed item and proceed with the remaining media archive files.

### 3. Hardcoded Config Path in Display Overlays settings save
*   **Severity**: Medium
*   **Affected Files**:
    *   [overlay.cpp](file:///P:/piTrove/src/overlay.cpp#L1036-L1182)
*   **Description**: When toggling settings through the direct physical on-screen display menu (e.g. enabling border or matting overlays), the configuration is saved via `g_cfg.save("/app/config/config.toml")`. If a custom configuration file path was provided on startup, these settings changes are written to the wrong file.
*   **Proposed Fix**: Update all on-screen configuration save operations in `overlay.cpp` to use the dynamic `g_cfg.loaded_path` value instead of the hardcoded path.

### 4. Data Race: Logging File Path Mutation across Threads
*   **Severity**: Medium
*   **Affected Files**:
    *   [util.cpp](file:///P:/piTrove/src/util.cpp#L297)
*   **Description**: The Logger constructor initializes the logging paths, but if the log folder is dynamically updated, the asynchronous background thread `Logger::flush_loop` reads `log_file_path` concurrently with potential writes from `Logger::init`. Accessing `std::string` objects concurrently from multiple threads without synchronization creates data races and memory corruption risks.
*   **Proposed Fix**: Protect all writes and reads to the log folder configuration strings inside the Logger structure using a dedicated `std::mutex`.

### 5. Infinite Hang on File Stat Queries During Network Dropouts
*   **Severity**: High
*   **Affected Files**:
    *   [scanner.cpp](file:///P:/piTrove/src/scanner.cpp#L61)
*   **Description**: In `stat_timeout()`, standard synchronous `stat()` is called on paths located on CIFS mounts. When network drops occur, standard `stat()` can block indefinitely in the kernel's network filesystem layer, hanging the main slideshow loop.
*   **Proposed Fix**: Implement `stat_timeout` using a non-blocking subprocess check or a thread-pool wrapper with a real timeout mechanism to abort stuck CIFS filesystem requests.

### 6. Data Race: Preloading Worker Queue State Inquiries
*   **Severity**: High
*   **Affected Files**:
    *   [preload.cpp](file:///P:/piTrove/src/preload.cpp#L316)
*   **Description**: In `PreloadQueue::worker_thread`, worker threads query queue capacity by calling `state->loaded_count.load() < state->max_size` without locking `state->work_mutex`. While `loaded_count` is atomic, the state transitions and queue structures are read/written concurrently, leading to potential race conditions on preloaded items.
*   **Proposed Fix**: Protect the queue capacity and work availability checks completely under the `work_mutex` lock in the worker thread loop.

### 7. SQL Resource Leak on WAL Initialization Error Paths
*   **Severity**: Medium
*   **Affected Files**:
    *   [cache.cpp](file:///P:/piTrove/src/cache.cpp#L63-L113)
*   **Description**: If any setup query (such as setting WAL mode or adding missing table columns) fails during database startup, the error path prints warnings and releases the error messages but leaves statements and database connections unclosed.
*   **Proposed Fix**: Implement standard cleanup guards (`sqlite3_finalize` and `sqlite3_close`) on every transaction setup error return path.

### 8. Division-by-Zero in Terminal UI Resizing Calculations
*   **Severity**: Medium
*   **Affected Files**:
    *   [tui.cpp](file:///P:/piTrove/src/tui.cpp)
*   **Description**: In the terminal config wizard, sizing math divides elements dynamically according to terminal columns. If the terminal is resized to extreme widths/heights, column math can produce a zero denominator, causing SIGFPE crashes.
*   **Proposed Fix**: Guard all element spacing and column offset divisions with bounds checks ensuring denominator values are at least 1.

### 9. Lack of Payload Size Validation on Google Photos Fetch
*   **Severity**: Medium
*   **Affected Files**:
    *   [google_photos.cpp](file:///P:/piTrove/src/google_photos.cpp#L301-L308)
*   **Description**: The sync manager downloads media items using curl but relies on basic filesystem existence. If curl returns an empty file (due to server errors or timeouts), the app proceeds to treat it as valid metadata or attempts to preprocess it, causing preprocessor decoders to crash on empty files.
*   **Proposed Fix**: Explicitly verify that the downloaded file contains a valid media header or minimum file size before incrementing successfully downloaded counts.

### 10. Preloading Thread Lifecycle Race Conditions
*   **Severity**: High
*   **Affected Files**:
    *   [preload.cpp](file:///P:/piTrove/src/preload.cpp#L69-L78)
*   **Description**: During preloader queue shutdown, workers are joined or detached with a 2-second timeout using `std::async`. If a worker thread is currently blocked on slow NAS I/O and gets detached, it can continue executing after the preloader queue object and its shared state are fully deallocated, leading to use-after-free crashes.
*   **Proposed Fix**: Keep worker threads joinable and rely on explicit thread tracking counters in shared state structures to prevent state deallocation while threads are still active.
