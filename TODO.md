# piTrove v5.7.0 — fork+execvp probe engine (May 18, 2026)

## Bugs Fixed in v5.7.0

| # | Severity | Bug | Fix |
|---|----------|-----|-----|
| 112 | CRITICAL | `probe_video_meta` hangs on corrupted/hung CIFS file — `popen("timeout 10s ffprobe ...")` blocks forever because `popen()`'s `fork()` + shell's `stat()` can block inside kernel CIFS path resolution before timeout even starts. 8s `timeout` is meaningless when the child process hasn't launched yet. | Replaced `popen()` with direct `fork()` + `execvp("ffprobe")`, reads stdout via `poll()` with hard wall-clock deadline. When deadline expires, parent calls `kill(pid, SIGKILL)` + `waitpid()` — `SIGKILL` interrupts any blocking syscall including kernel CIFS operations. Also added `RLIMIT_AS=256MB` on child to prevent ffprobe buffering entire video. `escape_single_quote()` helper removed — `execvp` takes raw `argv`, no shell quoting needed, any filename is safe. |

## Bugs Verified — Already Safe

| Bug | Status |
|-----|--------|
| `timeout` command on CIFS | Already broken — that's why we replaced it |

# piTrove v5.1.9 — Advanced architecture fixes (May 18, 2026)

## Bugs Fixed in v5.1.9

| # | Severity | Bug | Fix |
|---|----------|-----|-----|
| 110 | HIGH | SQLite WAL mode — `sqlite3_busy_timeout` never set; checkpoint races cause silent SQLITE_BUSY drops during caching | Added `sqlite3_busy_timeout(db, 5000)` after `sqlite3_open_v2` |
| 111 | HIGH | DRM master drop race — `drmDropMaster()` is async kernel request; fork+exec happens before kernel releases plane, causing mpv `[vo/drm] Failed to acquire DRM master` | Added `std::this_thread::sleep_for(25ms)` after `drmDropMaster` before fork |

## Bugs Verified — Already Safe

| Bug | Status |
|-----|--------|
| MPV event queue memory leak | Already safe — no `mpv_get_property_string` calls leak (only one call at line 1517, properly `mpv_free`'d) |
| VRAM leak from preload thread | Already safe — preload thread only loads `Image` (CPU RAM); VRAM conversion (`LoadTextureFromImage`) done on main thread in `Slideshow::update` |
| `getaddrinfo` / `freeaddrinfo` leak | Already safe — HTTP server uses hardcoded `sockaddr_in`, no `getaddrinfo` calls |
| `std::condition_variable` spurious wakeups | Already safe — all `cv.wait_for` calls use predicate lambdas (TimeoutState, first_img_cv, scan workers) |

## Bugs Fixed in v5.1.8

| # | Severity | Bug | Fix |
|---|----------|-----|-----|
| 106 | CRITICAL | TIFF 16-bit heap overflow — `MemAlloc(width * spp)` only allocates 1B/pixel; `TIFFReadScanline` writes `width * spp * (bit_depth/8)` bytes | Allocate `width * spp * (bit_depth / 8)` to match actual scanline size |
| 107 | HIGH | HEIC large-image pointer arithmetic overflow — `y * w * 3` is `int * int * int`, wraps for images > ~10K pixels | Cast to `size_t` before multiplication: `(size_t)y * w * 3` |
| 108 | CRITICAL | PNG inner `tmp_rgb`/`rows` leak (Fix 1 missed this) | Reuse outer `tmp_rgb`/`rows` variables instead of redeclaring |
| 109 | MEDIUM | WAL file unbounded growth — `journal_size_limit` never set | Added `PRAGMA journal_size_limit=10485760` (10MB cap) |

## Bugs Fixed in v5.1.7

| # | Severity | Bug | Fix |
|---|----------|-----|-----|
| 105 | MEDIUM | WebP 0x0 dimension — malformed WebP with valid headers but 0 width/height causes MemAlloc(0) → crash on memcpy | Added `w > 0 && h > 0` guard before allocation |

## Bugs Fixed in v5.1.6

| # | Severity | Bug | Fix |
|---|----------|-----|-----|
| 101 | CRITICAL | setjmp/longjmp memory leaks in libpng & libjpeg — `tmp_rgb`, `rows`, `scanline`, `rowbuf`, `img.data` leaked on corrupt image | Declared buffer pointers before setjmp, added cleanup in catch blocks |
| 102 | HIGH | `std::regex` recompiled per-file in `is_in_seasonal_window` and `is_month_in_window` — massive CPU thrashing | Made both regexes `static const` — compiled once |
| 103 | N/A | Main-thread stat() blocking on CIFS drop | Already handled — mtime captured during Phase 2 stat(), not in cache loop |
| 104 | HIGH | Unhandled thread exceptions in worker lambda → `std::terminate` | Wrapped outer worker loop in try/catch for both `std::exception` and `...` |

## Bugs Fixed in v5.1.5

| # | Severity | Bug | Fix |
|---|----------|-----|-----|
| 100 | MEDIUM | CacheManager missing bulk BEGIN/COMMIT transaction wrapping | Added `begin_transaction()`/`commit_transaction()` around Phase 3 loop |
| 99 | MEDIUM | Network yield only 1ms per file — insufficient on CIFS | Increased to 2ms per file |

## Bugs Fixed in v5.1.4

| # | Severity | Bug | Fix |
|---|----------|-----|-----|
| 98 | HIGH | Scanner still slow — `popen("find")` scans ALL files in matching folders | Replaced with smart month-folder filter: `is_month_in_window()` uses `ceil(window_days/30)` to calculate month spill-over, skips irrelevant months entirely before scanning |
| 97 | HIGH | `popen("find")` requires separate process — harder to reason about | Replaced with `std::filesystem::recursive_directory_iterator` in 3 threads — only iterates over pre-filtered month folders |
| 96 | MEDIUM | Fallback scans full directory if no month folders found | Added `_root_files` sentinel + root directory scan for loose files |
| 95 | MEDIUM | Folder regex didn't handle `YYYYMM` (no separator) format | Updated regex to `R"((\d{4})[-_]?(\d{2}))"` — optional separator |
| 94 | MEDIUM | Fallback for non-date folders (e.g., "Favorites") was missing | Returns `true` — non-date folders always scanned |

## Bugs Fixed in v5.0.1

(End of existing content follows)

## Bugs Fixed in v5.0.1

| # | Severity | Bug | Fix |
|---|----------|-----|-----|
| 93 | HIGH | Scanner ~1 file/sec — `std::filesystem::recursive_directory_iterator` does stat() per file (slow on CIFS) | Replaced with `popen("find")` — native subprocess, no per-file syscalls from our process |
| 92 | HIGH | Scanner scans ALL files even when most are outside temporal window | Added folder-level MM extraction + `is_month_in_window()` — only descends into folders whose MM falls within window |
| 91 | MEDIUM | Files outside window counted in UI counter | UI now shows only files passing seasonal window filter (correct behavior) |

## Bugs Fixed in v5.0.0

(End of existing content follows)

## Bugs Fixed in v5.0.0
