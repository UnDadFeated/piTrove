## v13.0.7 — Display Resolution Setting Reordering (June 16, 2026)

### Changed
- **Resolution setting priority** — Repositioned the resolution selection preset to the top of the display settings category in the terminal configuration console.

## v13.0.6 — Display Resolution Selection (June 16, 2026)

### Added
- **Interactive display resolution presets** — Added a resolution selector in the configuration console allowing the user to select between 1080p, 1440p, and 2160p resolutions, dynamically scaling display dimensions and photo textures.

## v13.0.5 — OSD Offset Defaults & TUI Terminal Robustness (June 16, 2026)

### Added
- **Default video OSD offset** — Adjusted default OSD offset values to position overlays optimally above the 1" matte for fresh installations.
- **TUI terminal raw mode safety** — Added End-Of-File (EOF) detection and signal protection to terminate the console menu loop gracefully on disconnects, preventing infinite CPU spinning.
- **Robust key sequence packet assembly** — Rewrote escape parsing to support longer CSI terminal escape sequences and buffer split packets in SSH sessions.

## v13.0.4 — Installer Verification & Guidelines Hardening (June 16, 2026)

### Added
- **Interactive directory confirmation** — Configured the installer to output a list of up to 10 files from the target directory during storage setup, validating paths for all local and network storage configurations.
- **Fail-safe fallback loops** — Integrated path validation loops that allow the user to easily adjust directories, credentials, or connection details if a path is invalid or empty.
- **Plan-first agent guidelines** — Hardened the agent guidelines to mandate structured implementation plans and user review before executing modifications.

## v13.0.3 — Network Recovery Safeguard (June 15, 2026)

### Added
- **Fail-safe network connection watchdog** — Hardened the connection monitor to automatically restart the wireless radio interface if the network gateway becomes unreachable, actively attempting to restore the link.
- **Automated system recovery reboot** — Configured an automatic system recovery reset if connection is lost continuously for more than 5 minutes, ensuring the device remains responsive and recovers cleanly from persistent access point drops.

## v13.0.2 — Installer Modernization & Sequencing Fixes (June 14, 2026)

### Fixed
- **Installer directory creation sequencing** — Bind-mount target directories (cache, config, logs, subtitles) are now created with correct ownership before the Docker build step, preventing Docker from silently creating them as root-owned.
- **Organizer TTY detection** — The media archive organizer now detects whether a terminal is available before requesting interactive Docker access, preventing failures when invoked from non-interactive contexts.
- **Cleaned up legacy step numbering** — Replaced stale numeric step comments with descriptive section headers throughout the installer for clarity.

## v13.0.1 — Video Playback Smoothness (June 14, 2026)

### Fixed
- **Reduced video buffering stalls over Wi-Fi** — Tuned the video player cache recovery threshold to resume playback faster after brief network hiccups. Increased I/O read chunk sizes to reduce round-trips when streaming from network-attached storage.

## v13.0.0 — Native Daemon Transition & Code Consolidation (June 13, 2026)

### Added
- **Native background connection monitor** — Integrated an automatic network adapter link recovery agent directly into the background engine. If the local gateway becomes unreachable, the slideshow manager automatically resets the interface to restore connectivity. This completely removes the reliance on external script daemons and scheduled host tasks.
- **Integrated media archive organizer** — Consolidated the media library organizer into a native compiled utility. Users can now reorganize archive folders chronologically or apply date prefixes in-place using the `--organize` command-line option directly on the system, eliminating external script execution overhead and dependency requirements.

### Fixed
- **Simplified system deployment** — Cleaned up installation requirements by eliminating external host scripts and scheduled system tasks.

## v12.6.0 — Automated Seasonal Fallback & Installer Streamlining (June 12, 2026)

### Added
- **Automated seasonal window fallback** — Implemented an automatic date fallback system. If a photo or video does not have chronological folder or filename date prefixes, the slideshow dynamically inspects embedded camera capture dates (EXIF metadata) and file creation/modification dates. This allows the seasonal window to automatically select and display photos from the correct time of year without requiring date organization.
- **New system diagnostics warning** — Added a specific diagnostic alert (`E808`) to notify when no date prefixes are found in the media library, showing that the system is using embedded file attributes to align the seasonal window.

### Fixed
- **Streamlined installation wizard** — Removed the interactive media reorganization prompt from the default installation workflow to prevent configuration delays. The media organizer utility remains available as a manual command-line execution option.
- **Installer auto-updates via cron** — Resolved an issue where daily scheduled update checks failed to execute from outside the repository directory by ensuring the updater switches to the repository directory before executing git commands.

## v12.5.5 — Advanced Diagnostics & System Integrity Safeguards (June 12, 2026)

### Added
- **Proactive local network status detection** — Engineered dynamic interface checks to monitor network adapter connection status and identify local connectivity drops. In the event of Wi-Fi or Ethernet disconnection, the system generates a clear network status error.
- **DHCP configuration & IP conflict warning** — Implemented address verification checks to detect when the system receives a self-assigned IP address. Warnings are raised with a specific configuration alert if DHCP leasing fails.
- **DNS lookup failure diagnostics** — Added explicit server resolution health checks during synchronization. If external domains are unreachable, the system triggers a DNS resolution alert, distinguishing network routing issues from server credential invalidation.
- **Critical storage threshold tracking** — Integrated storage capacity checks in the cache path and cloud sync loops, triggering a warning and pausing synchronizations if available disk space falls below a critical threshold (50MB).
- **Cache filesystem writability auditing** — Added cache folder write verification checks on application startup, alerting the user immediately if directories are mounted with read-only permissions.
- **Database transaction lock diagnostics** — Extended transaction error reporting to detect database locks or write blockages, triggering specific lock timeout or disk failure warnings.

## v12.5.4 — Reliability, Security, & Concurrency Safeguards (June 12, 2026)

### Added
- **Software watchdog recovery** — Integrated an active background watchdog system that monitors the health of the main slideshow loop. If the rendering engine stalls or freezes for more than 45 seconds, the watchdog triggers an immediate restart to restore operation automatically.
- **Non-blocking network mount verification** — Implemented lightweight TCP socket reachability checks for remote network shares. Before performing filesystem operations or media scans, the app verifies NAS status to prevent blocking kernel calls from freezing the interface.
- **Dynamic touchscreen hotplugging** — Upgraded touch input detection to periodically probe for active interfaces, permitting touchscreen controllers to be unplugged and reconnected at runtime without requiring an application restart.

### Fixed
- **Clean shutdown on network failure** — Refactored the media preloader and file monitoring threads to detach blocked filesystem workers upon application exit. The slideshow now terminates immediately and gracefully even if background threads are stalled on unresponsive network mounts.
- **Persistent MQTT integration** — Configured an automatic reconnect loop for remote messaging client subscriptions. If the connection to the broker drops or the broker is restarted, the client automatically attempts reconnection after 10 seconds.
- **Google Photos synchronization safety** — Added rigorous protocol and domain validations for dynamic download links, shielding the system from unauthorized web redirects. Prepend command qualifiers to all download requests to block command option injection.
- **Web dashboard capacity & resilience** — Expanded the remote web controller capacity to support up to 32 concurrent dashboard connections. Enforced active socket timeouts to shield the interface from sluggish or half-closed client connections.
- **Safe crash recovery handlers** — Eliminated dynamic heap allocations from system signals and terminate handlers. The app can now safely delete incomplete cache databases on crash without encountering secondary lockups or deadlocks.
- **Robust preloading queue lookup** — Restructured the background image buffer to allow out-of-order lookups. Skipping or seeking slides no longer discards successfully cached items, reducing redundant network fetches and load times.

## v12.5.3 — Network Resilience & Installer Robustness (June 12, 2026)

### Fixed
- **Wi-Fi keepalive reliability** — Scheduled the keepalive connection checks under root privileges to ensure the system is authorized to reset the wireless interface during connectivity dropouts.
- **Installer conflicts** — Restructured the installation sequence to clone the repository before writing configuration scripts, preventing directory conflicts. Added an automatic cleanup safeguard for incomplete/stale source directories.
- **Transient error diagnostics** — Media loading errors caused by network disconnections are now properly reported as network mount issues rather than image decoding failures.
- **Overlay message suppression** — Prevented network and file-decoding diagnostic boxes from flashing on screen during short, transient Wi-Fi drops unless the frame enters dedicated offline recovery mode.

## v12.5.2 — Interactive Installer Piped Execution Fix (June 11, 2026)

### Fixed
- **Installer keyboard input lock** — Resolved an issue where running the installation command directly via piped execution (such as `wget | bash`) would cause the installer to skip interactive prompt selections, default automatically, and terminate abruptly. Keyboard inputs are now correctly queried directly from the terminal console.

## v12.5.1 — Video Playback & Buffering Optimization (June 11, 2026)

### Fixed
- **Stutter-free video rendering** — Tuned internal video player options to increase network read-ahead buffering window (up to 120 seconds) and caching limits (up to 1024MB).
- **Smooth playback with no skipped frames** — Forced the video renderer to play every single frame sequentially, completely preventing timeline jumps or skips during high-bitrate playback.
- **Improved decoding performance** — Enabled fast decoding optimizations and skipped in-loop deblocking filters on complex frames, significantly reducing hardware CPU load when streaming high-resolution video clips over network mounts.

## v12.5.0 — Deep C++17 Modernization (June 11, 2026)

### Added
- **Zero-Allocation Number Parsing** — Replaced all exception-based string-to-number conversion with a unified template parser built on `std::from_chars`. Configuration loading and web dashboard parameter processing no longer allocate temporary heap strings or throw exceptions during parsing, reducing startup and runtime overhead.
- **Compile-Time Keyword Tables** — Media classification keyword arrays (documents, people, animals) and image extension lists are now evaluated at compile time. This eliminates per-launch heap construction and improves classifier throughput on the Pi's limited memory bus.
- **Expressive Date Parsing API** — Date extraction from filenames and metadata now uses `std::optional` returns with structured bindings instead of out-parameters, making missing-date paths explicit and self-documenting at every call site.
- **Deadlock-Safe Multi-Lock Acquisition** — Introduced `std::scoped_lock` for sites that acquire both playlist and configuration locks simultaneously, providing automatic deadlock-free ordering guarantees.
- **Modern Attribute Annotations** — Replaced legacy C-style `(void)result` suppression casts with `[[maybe_unused]]` attributes across system calls, subprocess launches, and iterator variables.
- **Expanded Zero-Copy String Parameters** — Extended `std::string_view` usage to seasonal window checkers, keyword matchers, and month-in-window filters, avoiding unnecessary string copies during scanning and playlist filtering.
- **Tighter Variable Scoping** — Applied C++17 if-with-initializer statements to configuration parsing, JSON value extraction, and URL parameter decoding, scoping intermediate variables to their exact point of use.

## v12.4.0 — C++17 Modernization & Directory Iterators (June 11, 2026)

### Added
- **Modernized Directory Scanner** — Transitioned directory scanning operations to standard C++17 directory iterators. This eliminates legacy platform-specific APIs and improves scanning stability across filesystem bounds.
- **Optimized String Handling** — Updated key utility helpers to use a memory-efficient string view interface. This avoids dynamic memory allocations and copies for parameters and string slices, lowering overhead during startup indexing.
- **Concurrent Configuration Sharing** — Upgraded the internal configuration synchronization system to permit multi-threaded concurrent reading. Telemetry dashboards, background preloading workers, and Home Assistant integrations can now access active settings concurrently without blocking one another.

## v12.3.0 — Global Linkage Modernization (June 11, 2026)

### Added
- **Global Variable Modernization** — Modernized the sharing system for application state, configurations, and core subsystems. This allows variable definitions to reside directly in header files, completely eliminating duplicate code definitions and streamlining compile-time link verification.

## v12.2.0 — Static Code Safety & Warning Hygiene (June 11, 2026)

### Added
- **Discarded Value Warning Checks** — Integrated strict return value checking compiler guidelines across all math, text parsing, and filesystem check operations to verify that logical values returned by system helpers are never silently ignored.
- **Unused Parameter Refinement** — Modernized inactive configuration stub endpoints and graphics engine callbacks to clean up legacy type casting directives, ensuring a warning-free compilation.

## v12.1.0 — Code Modernization & Configuration Safety (June 11, 2026)

### Added
- **Configuration Boundary Hardening** — Standardized value constraint clamping across all application subsystems to guarantee that out-of-bounds options supplied via manual configuration edits are safely and uniformly clamped to their defined limits on load.
- **Improved Media Cache Cleanup** — Refactored data structures inside the graphics engine to cleanly unpack metadata indices during folder scans and text-rendering cache clear-downs, improving stability and performance.

## v12.0.0 — Web Remote Refinements & Scroll Usability (June 10, 2026)

### Added
- **Configuration Help Hint** — Added a reminder below the dashboard settings form highlighting that more advanced configuration options are available via the `ssh pitrove config` command-line utility.

### Fixed
- **Diagnostics Log Scroll Lock** — Modified the log terminal window to only auto-scroll to the bottom when new events arrive if the user was already scrolled to the bottom. If the user scrolls up to read older log history, the console now maintains their scroll position, preventing the view from jumping down.

## v11.9.9 — Settings Expansion & Log Console Formatting (June 10, 2026)

### Added
- **Expanded Web Settings Control** — Introduced direct dashboard toggle switches for Touchscreen Mode, Shuffle, Ken Burns, Blurred Background, and Color-Matched Matte configurations.

### Fixed
- **Settings Label Clarification** — Renamed the generic volume slider in the remote dashboard settings panel to "Video Volume" for clear user guidance.
- **Log Console Font Sizing** — Reduced log viewer console text size to 0.65rem, optimizing information density and screen space utilization.

## v11.9.8 — Auto-Refreshing Remote Web UI Preview (June 10, 2026)

### Fixed
- **Web UI Preview Synchronization** — Implemented an automatic preview refresh trigger within the web remote dashboard. The dashboard now monitors active slide change events and dynamically reloads the visual preview image using cache-busting timestamp parameters and custom loading indicators, eliminating static visual state desync.

## v11.9.7 — API Validation & Log Diagnostics Hardening (June 10, 2026)

### Added
- **API Boundary & Clamp Checks** — Settings configuration updates via the web remote are now validated against safety limits (such as slide delay and video volume), triggering diagnostic error E807 if constraints are violated.
- **Log Stream Access Diagnostics** — Diagnostic log read checks now actively monitor log stream availability, generating error E126 if logs cannot be read.

### Fixed
- **Standardized Input Log Prefixes** — Standardized all background touchscreen event logs to consistently use the `TOUCH_INPUT` prefix for better readability.

## v11.9.6 — Installer Reliability Fixes (June 10, 2026)

### Fixed
- **Hardcoded home directory paths** — The media organizer and Wi-Fi keepalive scripts generated by the installer now correctly resolve the primary user's home directory instead of assuming `/home/pi`, preventing failures on systems with non-default usernames.
- **Web dashboard port mismatch** — Corrected the installer to reference port 9000 (matching the actual application default) instead of the stale port 8080 in all generated configuration, success messages, and Google Photos setup instructions.
- **Web dashboard always available** — The remote web dashboard is now enabled by default on fresh installs regardless of whether Google Photos integration is configured, matching the behavior of the default configuration template.

## v11.9.5 — Interactive Touchscreen Mode & Virtual Keyboard Overlay (June 10, 2026)

### Added
- **Touchscreen Mode Support** — Introduced a dedicated touchscreen capability, toggleable directly from the terminal configuration wizard. When active, tapping anywhere on the slideshow opens the quick configuration menu rather than advancing the slide.
- **Premium Numerical Virtual Keyboard** — Created an on-screen numerical keyboard overlay to easily enter interval delays and video volumes directly via finger touch, featuring a clean keys grid, backspace delete, and input validation.
- **Visual Settings Controls** — Added interactive increment/decrement buttons and a draggable volume slider track to the quick configuration menu overlay for quick, touch-friendly adjustments.

## v11.9.4 — Modern Grey Dashboard, Live Timer, and Popup Config Menu (June 10, 2026)

### Added
- **Modern Zinc Dashboard & Accent Themes** — Restyled the web HUD control interface with a modern, neutral gray (Zinc/Slate) appearance. Introduced user-selectable Light/Dark themes and color palette options (Zinc, Emerald, Sapphire, Amber) persisted client-side.
- **Direct-to-Device Quick Configuration Menu** — Implemented an overlay configuration menu that pops up on the display when right-clicking with a directly connected mouse. Allows immediate toggling of playback state, shuffle, interval delays, and screen blanking, with keyboard and mouse navigation support.
- **Live Slideshow Timer Display** — Exposed the active slideshow progression timer to the web dashboard, adding a live countdown badge that shows the seconds remaining before the next photo loads.

### Fixed
- **Dashboard Preview Routing Loop** — Corrected a route conflict that was causing the web remote to repeatedly trigger manual slideshow advance commands when loading preview images.
- **Preview Image Load Reliability** — Replaced socket stream writing logic with an interrupt-safe transmission loop to ensure complete, uncorrupted preview image deliveries.

## v11.9.3 — Consolidated Web Dashboard & Buffering Enhancements (June 10, 2026)

### Added
- **Consolidated Control Dashboard Layout** — Redesigned the interactive web control interface to reduce vertical spacing, merge fragmented telemetry boxes, and place all playback indicators and buttons into a single cohesive control card.
- **Enhanced 4K Video Buffering** — Optimized video playback buffer sizes and startup caching logic to enable stutter-free, smooth rendering of high-bitrate 4K@60fps video clips over local networks and NAS storage.

### Fixed
- **Documentation Spacing & Layout** — Cleaned up the documentation landing page by combining installation scripts and command lists into unified layout sections, removing redundant outline borders, and streamlining capabilities feature items.

## v11.9.2 — Interactive Web Settings & Diagnostics HUD (June 10, 2026)

### Added
- **Interactive Web Settings Control Panel** — Introduced an interactive configuration dashboard to dynamically customize slideshow preferences, overlay features, and automation settings directly from the web remote interface.
- **Live System Diagnostics Logs** — Integrated a real-time log terminal viewer on the dashboard to inspect system events, media indexing status, and diagnostic reports without requiring SSH terminal sessions.

## v11.9.1 — Safe Configuration Merging & Maintenance Updates (June 8, 2026)

### Added
- **Safe Configuration Merging** — Introduced an automated settings migration system during installation that merges the latest version defaults and new sections into your existing `config.toml` without overwriting any custom modifications.
- **Maintenance Command Flags** — Added and documented advanced command-line arguments in the installer to check/deploy updates manually or automatically via daily background schedules.

## v11.9.0 — Media Archive Reorganization Strategies (June 7, 2026)

### Added
- **Interactive Reorganization Menu** — Added an interactive strategy selection menu during media reorganization to choose between sorting files into chronological folders (`Photos/YYYY-MM/` and `Videos/YYYY-MM/`) or prefixing filenames in-place with dates (`YYYY-MM-DD_`) to maintain the existing directory structure.
- **In-Place Date Prefixing** — Integrated file sorting without changing path hierarchies, ensuring that existing folder configurations are preserved while enabling date-based temporal window features.

### Fixed
- **Installer Cron Job Registration** — Resolved an installer script pipeline failure under strict error handling when encountering environments with empty crontabs.

## v11.8.13 — Network Resilience & Keepalive Daemon (June 6, 2026)

### Added
- **Automated Network Keepalive Daemon** — Integrated a lightweight, persistent Wi-Fi keepalive script scheduled via cron that periodically monitors the local network gateway and automatically restarts the wireless interface via NetworkManager if unreachable, preventing long-term connection drops.

### Fixed
- **Resilient Media Outage Handling** — Hardened the image loading pipeline to verify media directory health and inspect filesystem error codes before flagging media files as corrupted in the cache database. This prevents transient network outages, rclone client restarts, or temporary NAS drops from marking valid files as bad.

## v11.8.12 — Resilient Offline Recovery & Failure Handling (June 5, 2026)

### Fixed
- **Stuck Slides on Media Load Failures** — Forced the slideshow to discard frozen images and immediately display the diagnostic recovery splash screen upon entering Offline Recovery Mode, resolving issues where slides would lock on screen during temporary network dropouts.
- **Fast Media Failure Skip** — Reduced the slide retry delay to 2 seconds (from 120 seconds) when encountering a single corrupted or missing image, preventing long pauses on frozen frames before advancing to the next item.

## v11.8.11 — Code Quality & Dead Code Cleanup (June 4, 2026)

### Added
- **Static Analysis Infrastructure** — Integrated general-purpose C++ static analyzers and diagnostic checkers on the Pi host compiler pipeline, ensuring long-term code quality and stability.

### Fixed
- **Dead Code and Resource Cleanup** — Removed 25 unused helper functions and dead/unused variables across the image decoding, directory scanning, and graphics rendering subsystems to streamline the application footprint.

## v11.8.10 — Seamless transition buffer presentation (June 4, 2026)

### Fixed
- **Seamless transition buffer presentation** — Resolved a single-frame black screen flicker occurring when slide transitions reached completion. Allowed the transition engine to render the final frame even after the active flag is cleared.

## v11.8.9 — Animated Pattern Blending (June 4, 2026)

### Added
- **Multi-Pattern Blending** — Added support to layer and blend up to 3 random static or animated patterns simultaneously. Exposed the blending limit preference as "Pattern Blend Count" in the TUI Display configurations, defaulting to a blend of 3 active patterns.

## v11.8.8 — Seamless Ken Burns transition completion (June 4, 2026)

### Fixed
- **Seamless Ken Burns transition completion** — Modified the Ken Burns transition engine to smoothly crossfade and zoom/pan back to 1.0x scale and centered alignment, preventing the visual size resize snap and jitter when transitioning to a static slide.

## v11.8.7 — Continuous OSD Overlay rendering (June 4, 2026)

### Fixed
- **Uninterrupted OSD Overlay rendering** — Drew on-screen overlays (filename, timer, clock, diagnostics HUD, on-this-day banners) directly over active transition frames, ensuring continuous OSD visibility and preventing black screen flickers when rotating slides.

## v11.8.6 — Background Matte Pattern Enhancements (June 4, 2026)

### Added
- **New Background Matte Patterns** — Expanded the background patterns to include triangles, polygons, squares, rectangles, hexagons, and fractals, both static and animated.
- **Mix & Match Pattern Style** — Added a combined background matte style that renders a randomized blend of multiple shapes dynamically.
- **TUI & Selection Integration** — Included all background matte pattern styles in the setup wizard, enabling direct selection of specific static or animated patterns.

## v11.8.5 — Automated Updates & Configuration Control (June 4, 2026)

### Added
- **Automated Update Checking** — Introduced a background auto-update scheduling system. The Digital Picture Frame can now be configured via the settings panel to automatically query the remote repository for updates and redeploy the container without manual intervention.
- **Git Branch Tracking** — Added support to select and track specific development channels (stable 'main' or active 'develop') directly from the settings wizard, aligning all background updates to the selected branch.
- **TUI & Installer Integration** — Exposed the Auto-Update and Update Channel selection preferences inside the System category of the terminal setup wizard.

## v11.8.4 — Terminal UI Compatibility & Options Loading Fixes (June 4, 2026)

### Fixed
- **Terminal UI category text visibility** — Enforced standard text background colors and color schemes in the setup wizard, ensuring option names and category tabs remain fully visible and readable when accessing the configurations from terminal clients with light backgrounds.
- **Terminal UI configuration options loading** — Resolved a layout rendering issue where configuration setting options failed to display in the wizard categories on the first screen redraw.

## v11.8.3 — Dynamic Midnight Temporal Rescanning (June 4, 2026)

### Added
- **Midnight Media Rescanning** — Enhanced the background watchman daemon to trigger an automatic directory scan at midnight to shift the active seasonal media window dynamically without requiring an application restart.
- **Unfiltered Media Scanner** — Modified the media directory scanner to process all files during the initial scanning phase, letting the playlist filters dynamically determine seasonal window eligibility at runtime.

## v11.8.2 — Database Error Handling Integration (June 3, 2026)

### Added
- **Database Health Error Reporting** — Integrated active database status checks to trigger dedicated system error codes (such as file open failures and database corruption flags) during initialization and scan sequences, automatically clearing the error states upon subsequent successful caches or fast-path database loads.

## v11.8.1 — Critical Stability & Display Fixes (June 3, 2026)

### Fixed
- **Null Cache Pointer Protection** — Hardened cache access routines throughout the media scanner and slideshow loop, preventing potential application crashes if the SQLite cache database is corrupted or fails to open during runtime.
- **Mismatched Transition Border Scaling** — Resolved a visual jump during image transition snapshot renders at resolutions other than 1080p by correctly scaling border widths.
- **Redundant Video Player Exit Checks** — Simplified video player termination state checks to ensure clean recovery and OSD error clearing.
- **Persistent Display Preferences** — Fixed an issue where display-related options (such as blurred backgrounds, color-matched mattes, opacity, and vignette parameters) were lost upon saving configuration updates.
- **Shadowed Rendering States** — Fixed a rendering issue where dual-portrait backgrounds could render with incorrect background clear colors during active transitions.

## v11.8.0 — Interactive Google Photos Setup and Fail-safes (June 3, 2026)

### Added
- **Interactive Google Photos Installer Prompts** — Added a dynamic step-by-step interactive configuration prompt for Google Photos in the setup installer script (`install.sh`), with fallback validation loops checking for empty client secrets, suffix structure formatting of client IDs, and valid integer range entries for synchronization periods.

## v11.7.13 — Default Animated Pattern and Visibility Enhancements (June 3, 2026)

### Added
- **Default Animated Pattern Style** — Changed the default pattern style from a static combined pattern to the active animated combined pattern.

### Fixed
- **Enhanced Pattern Contrast** — Increased default pattern brightness offset to 45 and optimized all rendering layers' transparency alpha levels to ensure clear visibility against color-matched backgrounds.

## v11.7.12 — Configurable Background Pattern Brightness (June 3, 2026)

### Added
- **Configurable Background Pattern Brightness** — Added a configuration parameter `pattern_brightness` and integrated it as "Pattern Brightness" under the Display category in the TUI Setup Wizard, enabling custom pattern contrast adjustment (0 to 150 offset) against the average color-matched background matte.

## v11.7.11 — Background Pattern Contrast Hardening (June 3, 2026)

### Fixed
- **Background Pattern Visibility** — Hardened the animated pattern backdrop to dynamically calculate background brightness and offset the pattern lines' brightness. This prevents the pattern from blending invisibly into the average color-matched background matte.

## v11.7.10 — Seamless Animated Pattern Background (June 3, 2026)

### Added
- **Seamless Animated Background Pattern** — Introduced a premium procedural ambient background matte mode. It generates multiple layers of intersecting lines moving slowly in opposite directions combined with gentle undulating waves, matching the average color of the active image to create a seamless, non-distracting, high-performance visual matte.
- **Default Background Configuration** — Set the new animated pattern background as the default display style.

### Fixed
- **Sideways Background Rendering** — Resolved an issue where blurred photo backgrounds were rendered sideways due to missing image orientation data. EXIF rotation attributes are now correctly applied to the background matte layout.

## v11.7.9 — System Stability & Bug Audit Fixes (June 3, 2026)

### Fixed
- **Slideshow Transition Selection** — Corrected an issue where customized transitions fell back to standard crossfades. Customized transition selections are now correctly respected and refreshed dynamically upon config updates.
- **Twin Portrait Layout Navigation** — Resolved a loop boundary issue where backward navigation on smaller playlists could fail to advance or land on the currently active slide.
- **Color-Matched Matte Borders** — Unified matte background configurations to ensure customized opacity settings are properly forwarded to matching backgrounds instead of defaulting to opaque colors.
- **Image Downscaler Stability** — Resolved a potential graphics crash or silent load failure when system resources prevented downscaling oversized images.
- **Media Preloader Memory Leak** — Patched a memory leak where image metadata reload events during slideshow playlist sweeps failed to release heap resources, leading to potential out-of-memory states.
- **File Extension Compatibility** — Hardened filename pattern matching to ensure longer file formats are fully supported under all platforms.
- **Installer Sequence & Reliability** — Reordered the system package installer sequence to perform root privilege and operating system validation checks before starting configuration or downloading external components.
- **Auto-Reconnect Mount Recovery** — Fixed an issue where the network mount retry routine stripped auto-reconnection parameters, preventing the media library from reconnecting after a network dropout.
- **DRM Display Controller Discovery** — Hardened active display card discovery during installation to fall back to standard GPU interfaces when connected monitors are asleep or unplugged.

## v11.7.8 — Resilient Image Loading & UI Smoothness (June 2, 2026)

### Fixed

- **Truncated Image Rendering Prevention** — Transitioned the image loading pipeline to fully read files into memory and validate read completeness before decoding. This completely prevents truncated or partially rendered images caused by temporary network disruptions on mounted network storage.
- **Background EXIF Processing** — Moved file metadata extraction and EXIF rotation parsing entirely into background preloading threads. This eliminates redundant disk access on the main thread, preventing UI freezes and frame stalls when transition deadlines are missed.

## v11.7.7 — 3D Edge Glow Shadow Option (June 1, 2026)

### Added

- **3D Edge Glow Shadow Mode** — Introduced a premium 3D edge glow shadow layout option. When active, it isolates the ambient backlight glow strictly to the right and bottom edges of the media container, creating a realistic, high-fidelity 3D drop-shadow effect on the digital frame. This option is enabled by default.
- **TUI & Config Wizard Integration** — Exposed the Edge Glow Shadow option in the Slideshow category within the TUI configuration engine, enabling instant visual control and seamless upgrades.

## v11.7.6 — Security Hardening and Parameter Sanitization (June 1, 2026)

### Fixed

- **Shell Command Injection Vulnerability Hardening** — Fully secured all dynamic shell execution entry points across the network services, dashboard callback routines, and media synchronization layers. Integrated robust shell-escaping and strict parameter validation on all configuration variables, client-supplied callbacks, and network headers, completely neutralizing unauthenticated shell execution risks.

## v11.7.5 — Hardware & TOML Syntax Diagnostic Triggers (June 1, 2026)

### Added

- **Hardware Telemetry Alarm Integration** — Integrated low-level thermal sensor reading blocks into the main slideshow overlay. The system now monitors the SoC core temperature in real-time, instantly triggering `E501` (System Overheating) diagnostic alerts if the temperature crosses 80°C, and auto-clearing the warning when the system cools back down.
- **TOML Config Parser Validation** — Hardened the default configuration loading routine by verifying line formatting structures. Malformed lines or invalid syntax in `config.toml` now immediately log and trigger `E801` (TOML Parse Failure) diagnostic warnings.

## v11.7.4 — Centralized Alphanumeric Diagnostic Logging (June 1, 2026)

### Added

- **Centralized Diagnostic Logging Subsystem** — Engineered a unified `trigger_error(CODE)` function that serves as the single source of truth for application diagnostics. Setting an active error code now automatically queries the C++ error database to log the formal error title, user-visible description, and troubleshooting recovery steps to the logs.
- **Subsystem Cleanup** — Removed duplicate redundant hardcoded warning strings, error logs, and duplicate output statements across the entire codebase (including the database caching engine, MQTT daemon, font rendering layer, remote control server, image loaders, video player routines, and Google Photos synchronizer), drastically reducing code complexity and footprint.

## v11.7.3 — Dynamic Deep Diagnostic Code Integration (June 1, 2026)

### Added

- **Dynamic Diagnostic Rendering** — Replaced all static error code rendering templates in the glowing HUD console overlay. The system now maps any integer error code dynamically to its corresponding alphanumeric database entry (`E###`), resolving formatting bottlenecks.
- **Deep Codebase Error Integration** — Hardened the application logic with broad integration of deep diagnostic tracking across multiple subsystems:
  - _Cloud Synchronizer_: Added real-time JSON parsing checks to trigger granular diagnostic warnings for OAuth client configuration failures, rate limiting, and private/missing album directories.
  - _SQLite Database Engine_: Integrated specific database alerts inside cache loading, WAL journal setups, migration routines, and statement preparation.
  - _Network Dashboard Server_: Configured dynamic warnings if the remote control daemon fails to bind to target socket interfaces.
  - _Home Assistant MQTT Integration_: Integrated active tracking if the daemon subscriber stream connection is rejected by the broker.
  - _Config Parser_: Integrated early warning parameters if the active TOML config files are unreadable or missing.

## v11.7.2 — Massive Diagnostic Error Catalog Expansion (June 1, 2026)

### Added

- **100+ Deep Diagnostic Error Codes** — Substantially expanded the system diagnostic warning subsystem with over 100 new 4-digit diagnostic codes (`E###`) to cover every layer of device operation:
  - _Storage & Network (E106 - E125)_: Added diagnostics for Ethernet link states, NTP sync drift, CIFS timeouts, routing failures, firewall blocks, socket exhaustion, Wi-Fi signal attenuation, and secure SSH tunnels.
  - _Media & Rendering (E206 - E225)_: Introduced deep tracking for EXIF parsing anomalies, unsupported color spaces, hardware H.265/HEVC decoder issues, FFmpeg stream analysis failures, twin layout mismatches, text margin overflows, and EGL transition failures.
  - _Cloud Integrations (E304 - E318)_: Expanded cloud monitoring to capture OAuth refresh token expiration, invalid/private albums, network drops during sync sweeps, payload formatting errors, and missing GCP API setups.
  - _Database & Local Storage (E404 - E418)_: Enhanced database diagnostics with codes covering SQLite constraint violations, read-only filesystem locks, WAL journal initialization errors, SD card bad sectors, disk block limitations, and temporary filesystem exhaustion.
  - _Hardware & Operating System (E503 - E518)_: Integrated hardware alerts covering power supply undervoltage warnings, GPU out-of-memory flags, hardware watchdog timeout alarms, missing udev rule contexts, GPIO bus failures, RTC battery drainage, and obsolete kernel firmware drivers.
  - _Graphic Pipeline & Framebuffer (E604 - E618)_: Added deep graphics logging for DRM overlay plane allocation failures, cursor texture creation issues, lost DRM master locks, screen blank timeout malfunctions, and EGL configuration mismatches.
  - _Smart Home & Integration (E703 - E718)_: Expanded MQTT tracking to handle payload parsing errors, JSON schema format violations, heartbeat timeouts, broker authentication rejections, certificate handshake errors, and Client ID collisions.
  - _Lifecycle & Config (E801 - E806)_: Added validations for TOML syntax errors, missing configuration blocks, lockfile write issues, invalid startup flags, thread pool stalls, and unclean application shutdowns.

## v11.7.1 — Diagnostic Error Catalog Expansion (June 1, 2026)

### Added

- **Dynamic Error Database Subsystem** — Extracted the error catalog definitions into a dedicated error database module. This decouples the seed details from core database caching logic for better maintainability and modularity.
- **Comprehensive Error Catalog Expansion** — Expanded the error catalog to over 20 distinct 4-digit diagnostic E### codes spanning every architectural layer:
  - _Storage & Network (E101 - E105)_: Outlining NAS mount failures, Wi-Fi drops, IP address conflicts, port binding issues, and DNS resolution failures.
  - _Media & Rendering (E201 - E205)_: Detailing decoding errors, video player crashes, invalid aspect ratios, missing TTF assets, and audio device failures.
  - _Cloud Integrations (E301 - E303)_: Describing Google Photos authentication issues, API rate limits, and invalid Client ID credentials.
  - _Database & Caching (E401 - E403)_: Detailing SQLite database corruption, transaction lock timeouts, and critical host disk space warnings.
  - _System & Hardware (E501 - E502)_: Telemetry warning for critical SoC overheating and RAM allocation exhaustion.
  - _Graphics Pipeline (E601 - E603)_: Informing on missing modesetting HDMI connectors, EGL swap page flips, and SDL frame rendering failures.
  - _Home Assistant (E701 - E702)_: Outlining MQTT broker unreachability and sensor subscription failures.

## v11.7.0 — Google Photos Cloud Integration & Diagnostic Warning HUD (June 1, 2026)

### Added

- **Google Photos Cloud Sync** — Designed and implemented a background Google Photos integration that synchronizes cloud media files directly to the local cache. Includes full C++ synchronization logic, popen-based curl requests for media item metadata retrieval, and image downloading.
- **Glassmorphic OAuth Setup Wizard** — Built a professional Glassmorphic setup wizard served at `/google_photos_setup` via the built-in HTTP server. The wizard guides users to link their Google Photos credentials, handles Web OAuth authorization flow redirects, executes the token exchange, and secures the refresh token.
- **Diagnostic Warning HUD & Error Catalog** — Integrated a premium 4-digit diagnostic warning overlay box featuring glowing phosphor-red frames. Seeded an SQLite-based diagnostic error catalog database with troubleshooting instructions (E101 to E401) to assist in quick diagnostics (such as empty NAS mounts, corrupted files, and video player crashes).
- **GPhotos Category Tab in TUI** — Appended a dedicated category tab `"GPhotos"` to the terminal configuration wizard to view and toggle Google Photos status, sync interval, and credentials.
- **Universal ASCII TUI Rendering** — Converted all UTF-8 characters and box-drawing elements within the configuration wizard to universal ASCII characters, ensuring flawless TUI display when connecting from Windows clients via SSH/Command Prompt.

## v11.6.5 — Network Mount Persistence Fix (May 31, 2026)

### Fixed

- **Persistent Network Storage** — Resolved an issue where network storage mounts would automatically disconnect after short periods of inactivity. The system now maintains a persistent connection to the NAS, preventing slideshow interrupts, file loading failures, and unexpected offline recovery screens during transitions or long idle delays.

## v11.6.4 — Network Mount Persistence & SSH Keep-Alives (May 30, 2026)

### Added

- **SSH Keep-Alives in Installer** — Updated the premium graphical installer to persistently configure server-side SSH/SFTP keep-alives (60-second intervals) on new Debian Trixie installations. This prevents connected remote mounts, command terminal links, and network filesystem mounts (like rclone connections from Windows clients) from dropping or freezing during periods of inactivity.

## v11.6.3 — Aggressive Video Buffering & Network Smoothness (May 29, 2026)

### Added

- **Aggressive Network Video Caching** — Forced enabling of aggressive read-ahead caching for video playback. By increasing the memory cache buffer limit to 150 Megabytes and the demuxer read-ahead window to 20 seconds, the media engine completely mitigates network latency, bandwidth fluctuations, and temporary throughput bottlenecks when streaming high-bitrate HEVC/H.264 video files over Wi-Fi and network mount points. Macroblock corruption ("noise"), pixelation, and playback stutter during complex video scenes are now eliminated.

## v11.6.2 — Transition Texture Alpha Blending Fix (May 29, 2026)

### Fixed

- **Abrupt Transition Pops** — Resolved an issue where next images would abruptly pop onto the display without blending/fading. Transition target textures now explicitly set the SDL blend mode to alpha blending (`SDL_BLENDMODE_BLEND`) upon creation, ensuring alpha modulation functions correctly for smooth fades in and out.

## v11.6.1 — Transition Freeze & Loop Lock Fix (May 29, 2026)

### Fixed

- **Slideshow Transition Freeze** — Fixed a regression introduced by the image-load lag time reset, which checked the active transition progress and inadvertently locked it in an infinite loop at exactly 0.0f. Transition start is now detected using a one-shot target texture creation check, allowing the elapsed time delta to safely advance slideshow transitions.

## v11.6.0 — Full-Screen Transition Scaling Fix (May 29, 2026)

### Fixed

- **Double-Fitting Transition Shrinkage** — Resolved a layout bug where slides would suddenly shrink, showing a 2" black matte during transitions, and leaving the active slide stuck without overlays. Every transition effect now draws pre-rendered target frames at full-screen proportions, ensuring seamless, continuous edge-to-edge backdrop rendering.

## v11.5.9 — Edge-to-Edge Average Color Matte Backgrounds (May 29, 2026)

### Added

- **Edge-to-Edge Color-Matched Matte Backgrounds** — Seamlessly extended the color-matched average background to fill the entire screen edge-to-edge. Only the photo, its 3D miter border, and ambient glow are now fitted inside the 1" matte area, eliminating black borders and creating a cleaner visual experience.

## v11.5.8 — Video Overlay Margin Restoration (May 29, 2026)

### Fixed

- **Standard Matte Margin Restoration** — Reverted the video player's OSD and subtitle margins to the classic 1" matte boundaries (matting size + offset), maintaining unified proportions across video playback.

## v11.5.7 — Transition & Crossfade Hardening (May 29, 2026)

### Added

- **Seamless Video-to-Photo Fades** — Overhauled slideshow state transitions when finishing or skipping video items. Stale image states are cleanly purged and the EGL display is swept to black immediately upon video termination, completely eliminating stale photo flashing and enabling beautiful, cinematic fades from black.

### Fixed

- **Slideshow Transition Stutter & Fast Swapping** — Resolved a timing discrepancy where transitions between photos would occasionally swap instantly without crossfading. The frame-timer is now dynamically reset at the start of active animations, safely discarding the blocking duration spent loading images from disk and ensuring smooth, 60 FPS transitions.

## v11.5.6 — Video Overlay Alignment (May 29, 2026)

### Fixed

- **Video Filename Alignment** — Dynamically positioned the video filename overlay on mpv playback to sit at the exact same vertical and horizontal coordinates as the image filename overlay in the bottom left, ensuring a perfectly aligned and seamless presentation across both media types.

## v11.5.5 — Watchdog and MQTT Pipeline Hardening (May 29, 2026)

### Added

- **Outbound Message Queue Capping** — Hardened the message publishing client against network dropouts by capping the outbound message queue. High-frequency or duplicate status updates are gracefully discarded when offline, preventing memory depletion and ensuring immediate responsiveness upon network recovery.

### Fixed

- **Watchdog Midnight Hang** — Resolved a vulnerability where the background time-monitoring watchdog would attempt to verify remote network storage accessibility during active network dropouts. Accessibility checks are now skipped during recovery mode, preventing the background monitor thread from stalling.

## v11.5.4 — Network Resilience & Offline Recovery Mode (May 29, 2026)

### Added

- **Offline Recovery Mode** — Integrated an automatic, elegant recovery state machine to handle sudden network drops or NAS mount disconnects. If the system experiences three consecutive media load failures, it automatically switches to Offline Mode, displaying the system splash screen with a red `[OFFLINE] Reconnecting to NAS...` status banner instead of a blank black screen.
- **Resilient Fallback and Back-off Timers** — Implemented an adaptive back-off delay during network outages. The slideshow slows down filesystem checks to a stable 30-second interval, keeping the main presentation thread responsive to keypresses and remote web remote commands while waiting for the network connection to recover.

## v11.5.3 — Installation and Configuration Wizard Stability (May 29, 2026)

### Fixed

- **DRM GPU Card Detection** — Corrected a sequence error in the installer where system GPU hardware values were written to configuration files before probing took place. The installer now persistently maps the correct physical display controller on startup.
- **Concurrent Configuration Editing** — Resolved a conflict where opening the interactive settings wizard while the picture frame was actively running would cause startup collision errors. Setting edits are now seamlessly dispatched while the background loop is active.
- **Mount Point Cleaning** — Hardened the network storage configuration checks to cleanly purge existing entries of the same mount point from the system filesystem table, preventing duplicate mount listings.

## v11.5.2 — Hardening, Performance & Security Optimization (May 29, 2026)

### Fixed

- **Deadlock inside Adaptive Text Rendering** — Resolved a critical deadlock that could freeze the application when rendering overlays with adaptive, contrast-aware colors.
- **Shell Injection Security Safeguards** — Fully secured all MQTT shell command invocations by strictly sanitizing and escaping broker paths, topics, payloads, usernames, and passwords.
- **Ambient Lighting Dimming Bug** — Fixed an issue where average color calculations for the bottom edge strip were incorrectly scaled down by a wrong sample divisor, resolving a visual bug where bottom bias-lights and miter borders appeared artificially dimmed.
- **HTTP Server Connection Pool Exhaustion** — Added robust read/write socket timeouts to prevent slow or hanging HTTP clients from starving the web dashboard's connection pool.
- **Stuck Log Rotation** — Restructured debug logging rotation to ensure that after rolling over a size threshold, a new timestamped file is correctly opened, preventing the logger from getting permanently locked in fallback output files.
- **Child Process Leakage** — Prevented thread and resource accumulation when video information probes timed out, guaranteeing immediate, synchronous cleanup of subprocesses.
- **Configuration Boundary Safety** — Implemented strict clamping boundaries for video playback volume in the configuration file parser to match the safety limits enforced in the terminal wizard.
- **Robust Playlist Navigation** — Simplified backwards playlist index tracking to prevent arithmetic overflow and ensure smooth, wrap-around navigation when returning to previous slides.

## v11.5.1 — System Hardening, TUI Bounds & Concurrency Gaps (May 29, 2026)

### Added

- **Exposed Maximum Brightness in Terminal UI** — Exposed `Max Brightness` (integer setting, 0–100 range) to the `Advanced` category of the terminal settings wizard, providing full visual control over maximum backlight illumination limit.

### Fixed

- **Terminal UI Gaps and Sizing** — Restored complete access to previously hidden or cut-off settings like `HTTP Timeout`, `HTTP Bind attempts`, and `Reset Cooldown` by calculating terminal categories dynamically rather than relying on hardcoded arrays.
- **Safety Boundary Clamping** — Applied strict range clamping on user-entered values in the terminal configuration utility, ensuring valid rotation angles and keeping delays, volumes, durations, thread counts, and scanning parameters within safe operating thresholds.
- **Thread Concurrency Safety** — Synchronized background status message publishing and thread tear-down logic, preventing rare race conditions that could lead to abnormal termination during rapid system reloads or shutdown.
- **Cleaned Up Dead Code** — Purged duplicate headers and stale, orphaned color options from the terminal configuration utility to keep compilation warning-free.

## v11.5.0 — Compiler Warning Resolution, TUI Borders & Preload Matte Fix (May 28, 2026)

### Added

- **Exposed 3D Border in Terminal UI** — Exposed direct configuration switches for the 3D picture frame border (`3D Border` and `3D Border Width`) inside the Terminal configuration wizard Display tab. This allows users to completely toggle or adjust 3D miter borders independently of ambient glow effects.

### Fixed

- **Preloaded Matte Colors** — Resolved a visual bug where preloaded images defaulted to solid black matte borders instead of rendering color-matched backdrops. The background preloader now seamlessly copies precomputed average colors into active drawing pipelines.
- **Subsystem Warnings and Stability** — Silenced remaining compile-time warnings, removed defunct prototypes, and locked startup configuration reads to harden application stability and concurrency.

## v11.4.7 — Graphics & System Cache Relocation (May 28, 2026)

### Added

- **Centralized System and GPU Caches** — Re-routed all container graphics shader caches (Mesa/GPU) and system caches to persist inside the dedicated cache directory (`/home/pi/piTrove/cache/`) rather than the host user's home directory.

## v11.4.6 — System Hardening (May 28, 2026)

### Fixed

- **Subsystem Warnings and Type Safety** — Resolved minor compilation warnings, applied type-safety void casts, and cleaned up redundant default initializations to enhance stability.

## v11.4.5 — Config Extractions & Minor Fixes (May 28, 2026)

### Added

- **Configurable max texture dimension** — Added `max_texture_dim` config option (under `[video]`, default 1920) to control the maximum image dimension in pixels for texture scaling.
- **Configurable HTTP timeout & bind attempts** — Added `http_socket_timeout` (default 2s) and `http_bind_attempts` (default 10) under `[video]` for HTTP server tuning.

### Fixed

- **TUI category bar missing MQTT** — Category bar loop was hardcoded to 9 items, hiding the 10th "MQTT" category tab.
- **Undefined behavior in cache upsert** — Replaced `const_cast<MediaItem&>` with `mutable` keyword on `is_camera` field.

## v11.4.4 — Init Lock, CIFS Hang & Double-Lock Fixes (May 28, 2026)

### Added

- **Configurable mpv OSD offset** — Added `osd_offset_x` and `osd_offset_y` config options (under `[video]`) to adjust MKV video overlay position in pixels.

### Fixed

- **Slideshow freeze on startup** — Removed `init_lock` that acquired `g_playlist_mtx` without releasing before the main loop, causing the slideshow loop to block forever on its first mutex acquisition.
- **Slideshow freeze on CIFS mounts** — Replaced `file_exists()` (CIFS `stat()` hang) in the main-loop missing-file check with simple `current_idx` bounds validation. Deleted files are caught on the next load failure instead.
- **Slideshow freeze after first frame** — Removed redundant `lock_guard<std::mutex> pl_lock(g_playlist_mtx)` inside the preload section that deadlocked because `playlist_lock` already held the same mutex (introduced in Batch #21).
- **HTTP /api/status timeout** — Resolved as a consequence of fixing the double-lock deadlock; the API no longer blocks trying to acquire `g_playlist_mtx` held by the stuck main thread.

## v11.4.3 — Pipeline Lock & CIFS Hang Fixes (May 28, 2026)

### Fixed

- **Slideshow freeze on CIFS mounts** — Filesystem existence checks are now performed outside the playlist mutex, preventing a hung CIFS `stat()` call from blocking the entire slideshow pipeline and leaving the splash screen frozen on the display.
- **"AWAITING I/O PIPELINE... BLOCKED" splash stalling** — Cleaned up orphaned SQLite WAL/SHM files that prevented the cache from initializing, keeping the display stuck on the scanning splash.
- **Splash shows real item count on fast-path restart** — The splash no longer briefly flashes "FILES FND: 0" when loading from a populated cache. Shows the actual cached count instead.
- **Cache-complete confirmation frame** — A final "CACHED: N" frame is displayed for 500ms after the cache build finishes, so the user sees the finished state before the slideshow begins.

## v11.4.2 ? Scanner Progress & TUI Timers Fixes (May 28, 2026)

### Fixed

- **Scanner progress counter** ? Restored live file counting during initial scan so the splash screen shows "FILES FND: N" incrementing in real time instead of staying at 0.
- **TUI flash message timer** ? Flash messages now expire based on the time they were triggered rather than the TUI start time, preventing messages from disappearing prematurely.
- **TUI category bar** ? Fixed a hardcoded loop bound that rendered only 9 of 10 categories, making the MQTT category invisible and unreachable.
- **Playlist lock race on item type** ? Captured g_eligible[next_idx].type under the playlist lock to prevent data races with the watchman thread that could cause use-after-free.
- **Twin portrait race** ? Wrapped should_be_twin_portrait call under the playlist lock to prevent concurrent vector mutation.
- **Mpv process hang** ? Added 5-second SIGKILL fallback timeout when waiting for mpv child process to terminate.
- **On-this-day config snapshot** ? Added missing config mutex lock when reading show_people_faces and keep_animals in the ON_THIS_DAY filter path.
- **HTTP connection limit race** ? Replaced TOCTOU check-then-act pattern with single atomic etch_add + rollback for connection limiting.
- **HTTP thread exception safety** ? Added try-catch in tracked thread lambda so thread handles are always released on exception.
- **Edge sampling asymmetry** ? Fixed bottom edge pixel sampling to use 3 rows (matching the top edge) instead of a broken d=-1..+1 loop that yielded only 2 samples.
- **Config logger guard** ? Added is_initialized() check before calling g_logger.warn during config load to prevent early-initialization log loss.
- **Splash counter format** ? Removed zero-padded %06d format from FILES FND and CACHED display strings.

# Changelog

## v11.4.1 — Concurrency, Stability & Cooldown Persistence (May 28, 2026)

### Added

- **Persistent Slide Cooldowns Across Restarts** — Hardened the media scanning pipeline to fully preserve the recently shown history cooldowns across application restarts by default, maintaining rich slideshow variety under all reload scenarios.

### Fixed

- **Slideshow Hanging during Debug Log Rotation** — Resolved a high-severity deadlock in the debug log subsystem that would freeze the slideshow during file rotations or after long running sessions.
- **Asynchronous Message Queueing** — Refactored status publishing to process messages via a dedicated background task queue, completely eliminating detached background threads and preventing application termination hangs.
- **Background Web Client Management** — Integrated safe tracking and automatic joining of web dashboard client threads during shutdown, ensuring clean, crash-free application exits.
- **Interruptible Motion Sensor Simulation** — Hardened the remote dashboard's simulated motion trigger delay to exit immediately on system shutdown, avoiding background thread exit races.
- **Safe Transition Skipping** — Implemented strict validation checks for in-flight image textures during rapid slide skips, preventing slideshow crash loops.
- **Monospace Font Missing Safety** — Added safety fallbacks to the overlay dashboard renderer when custom monospace fonts are missing or unreadable on the host system, avoiding startup failures.
- **Off-Frame Display Control** — Swapped display sleep commands to run synchronously off-frame, eliminating background thread races during screen power transitions.
- **Safe Time Parsing Fallbacks** — Hardened the temporal seasonal window parser and media watchman monitor with robust validation checks against system clock translation failures, ensuring reliable scheduling.

## v11.4.0 — Codebase Hardening & Stability (May 28, 2026)

### Fixed

- **Startup Thread Lockups** — Resolved a concurrency deadlock during slideshow initialization by securing the initial media load sequence under the global playlist lock, ensuring the slideshow starts reliably even when media files are slow to load.
- **System File Descriptor Leak** — Corrected a resource leak where extraneous file descriptors were left open during background media probe launches, preventing system-wide file handle exhaustion.
- **Zombie Video Player Processes** — Ensured video playback processes are cleanly resumed before termination signals are dispatched. This allows suspended video streams to shut down gracefully and release system graphics resources, eliminating stuck video frames.
- **Corrupt Image Crash** — Hardened image orientation parsing to prevent application crashes when encountering images with malformed or corrupt metadata headers.
- **Web Dashboard Lockups** — Applied absolute write timeouts to dashboard client connections. Slow or disconnected remote control browser instances can no longer indefinitely lock HTTP server ports or freeze the web dashboard.
- **Precise System Telemetry** — Standardized CPU temperature and database metric parsing to output precise, consistent decimal formatting on the system status screen.
- **TUI Division-by-Zero and Configuration Bounds** — Implemented strict upper/lower bounds checks for custom ambient backlight strength settings and empty lists in configuration menus, eliminating potential graphics color overflow glitches and menu crashes.
- **Numeric Directory Name Overflow** — Restrained numeric folder date parser loop limits, protecting the scanner against signed integer overflow errors when encountering exceptionally long numbers in folder or file names.

## v11.3.10 — Concurrency, Thread Safety & Graphics Hardening (May 27, 2026)

### Fixed

- **Row-by-Row Splash Screen Pixels Copy** — Replaced direct contiguous memory copies when loading the boot splash screen with a row-by-row memory copying procedure that honors pitch-alignment variables. This ensures correct, distortion-free splash screen display regardless of GPU/driver-specific pixel alignments.
- **Background Preload Epoch Filtering** — Added a dynamic epoch-generation tracking counter to the background image loader's preloading queue. When the slideshow advances or changes directories, the queue cancels in-flight jobs and rejects stale worker-thread decodes upon completion, completely preventing queue blockage and memory waste from obsolete preload operations.
- **DRM Master Transition Page-Flip Synchronization** — Integrated explicit hardware graphics client and drawing synchronization commands before dropping the display driver master locks to transition between standard rendering and standard video playback. This completely eliminates screen freezes, GPU page flip hangs, and modesetting locks on high-resolution displays.
- **Safe Image Downsampling Boundaries** — Hardened the low-resolution nearest-neighbor downsampler to strictly validate scaling bounds and clamp coordinates to positive bounds, preventing negative index offsets or out-of-bounds pixel array reads on warped or miniature images.
- **Rigorous Graphics Error Resets** — Implemented drawing context error clearing commands prior to critical graphics surface and texture creation tasks, and added rigorous error validation to log and troubleshoot system drawing malfunctions immediately.
- **Terminal UI Sizing Warnings** — Hardened the interactive terminal configuration wizard to validate display grid parameters and clamp minimum formatting widths, preventing text overflows and malformed layout tables when shrunk to tiny dimensions.
- **Strict Border Width Validation** — Constrained the custom picture frame border width configurations to safe, positive maximum limits, ensuring borders never overlap or cover collage composite mattes.
- **Self-Healing Folder Monitor Mount Checks** — Upgraded the background calendar monitor loop to verify read-access permission on the media repository before executing midnight playlist swaps. This prevents blanking or corrupting active slideshow playlists during transient network storage or NAS mount disconnects, automatically retrying once connection is restored.
- **Dynamic Video Player Display Probing** — Enhanced the dynamic display detector to dynamically query the active connected HDMI port in the media player backend when set to automatic mode, preventing incorrect target connector fallbacks and redundant display querying logs.

## v11.3.9 — Resource Safety and Concurrency Hardening (May 27, 2026)

### Fixed

- **Persistent HTTP Server Ports** — Prevented configuration reloads from overwriting dynamic fallback ports in memory, ensuring the web dashboard continues running seamlessly even when the default port is occupied.
- **Robust Cache Database Purging** — Hardened the cache database recovery mechanism to delete auxiliary SQLite WAL and SHM journal files alongside the database file upon detecting corruption or incomplete loads, preventing stale logs from corrupting newly initialized databases.
- **Hardened Media Path Parser** — Fixed a file extension classification issue where directory paths containing dots (e.g. date-based folders) were incorrectly processed by the media scanner, ensuring extensionless files inside such directories are skipped cleanly.
- **Safe Transaction Management** — Added transaction state safety checks to the database manager, completely eliminating transactional conflicts, double-starts, or redundant rollback actions during bulk database operations.
- **Race-Free Socket Shutdown** — Refactored the web control server shutdown sequence to use socket shutdown signals, allowing the background thread to safely close the descriptor and preventing file descriptor reuse races with other threads.

## v11.3.8 — Self-Healing Network Storage & Smooth Video Transitions (May 27, 2026)

### Added

- **Self-Healing Network Storage Mounts** — Upgraded network storage mounting parameters to utilize systemd-managed automounts with robust connection timeout settings. This ensures the digital frame cleanly bypasses transient Wi-Fi drops without causing system-wide filesystem lockups, seamlessly recovering and remounting the media library once the network is restored.
- **Persistent Wi-Fi Power Management** — Enforced hardware-level Wi-Fi power-saving overrides via a persistent system udev rule, keeping the wireless interface actively connected during long slideshow idle periods.

### Fixed

- **Smooth Photo-to-Video Transitions** — Integrated the slideshow transition engine with video playback initialization, running a smooth visual transition from the current photo to a black canvas before launching the media player. This eliminates sudden, abrupt cuts to black while the media player process initializes.
- **Transition Memory Optimization** — Prevented unneeded background image-decoding attempts on video assets during transition lookaheads, optimizing system memory usage and reducing network disk load.

## v11.3.7 — Reliability, Concurrency, and Shown-History Persistence Option (May 26, 2026)

### Added

- **Configurable Shown-History Reset** — Introduced a new slideshow setting to control how the frame handles shown history on startup. By default, the system persists the recently shown photo cooldowns across restarts to preserve variety. Users can now enable the "Reset Cooldown" toggle in the settings wizard to optionally clear all shown histories on restart for a completely fresh slideshow cycle.

### Fixed

- **Background Preloader Offloading** — Moved all matte color and image-average color analyses from the main rendering loop into the background loading threads. This avoids CPU spikes on image transitions and maintains a rock-solid, fluid 60 FPS visual experience.
- **Improved Dissolve Transition Rendering** — Optimized the dissolve visual transition to draw scattered pixels in single batched graphics calls. This completely eliminates rendering lags and stuttering during crossfades on low-power devices.
- **Hardened Remote Server Concurrency** — Upgraded the background web remote controller to use atomic socket handling. This prevents data race conditions and accidental socket sharing during service restarts or connections.
- **SQL Transaction Integrity** — Added automated rollback mechanisms for settings saves and metadata updates. This prevents database transaction lockups on full or restricted filesystems, keeping setting writes highly reliable.
- **Media Player Debugging** — Redirected media player diagnostics to standard log folders, ensuring video playback logs are safely captured inside privileged environments.
- **Installer Layout Formatting** — Fixed a stray visual character layout alignment in the branch selection prompt during terminal installation.

## v11.3.6 — Stability, Performance, and Transition Optimizations (May 26, 2026)

### Added

- **Dynamic Settings Reloading** — Added dynamic settings reloading support to the active slideshow loop. The digital frame now listens for settings changes and applies them immediately without needing to restart the background daemon.

### Fixed

- **Pixelate Transition Performance** — Optimized the pixelated transition renderer to batch drawing commands efficiently. This eliminates CPU-bound frame rate drops and stuttering during transition sequences, ensuring liquid-smooth rendering even with small pixel blocks.
- **Background Blur Efficiency** — Re-engineered the box blur backdrop renderer to cache pre-calculated graphics textures and optimize resolution scaling. This reduces the background memory footprint by 94% and improves blur rendering speeds by 16x, eliminating GPU-to-CPU stalls during image loads.
- **Background Preloader Deadlock** — Hardened the background image loader's preloading queue to discard stale or mismatched preloaded image requests, preventing preloader deadlocks and ensuring a continuous stream of images.
- **Robust Font Management** — Implemented reference tracking on custom typography resources to guarantee global graphics interfaces are only closed when the final rendering instance is destroyed. This prevents premature font shutdowns and potential visual artifacts.
- **Micro-Image Handling** — Protected the color extraction parser against tiny/empty media files (less than 3 pixels wide/tall), preventing out-of-bounds rendering crashes when displaying corrupted or miniature images.
- **MQTT Concurrency Hardening** — Secured the smart-home publisher threads against race conditions during system shutdowns, preventing double-closing of communication pipes and boosting background daemon stability.
- **Seasonal Date Pattern Support** — Extended the smart calendar window to support separator-less date patterns (like YYYYMM and YYYYMMDD), making it easier for users to automatically filter custom photo directory naming patterns.
- **Terminal UI Resilience** — Hardened the settings wizard window measurement utility, allowing the interactive wizard to load cleanly with robust default fallback dimensions even when run inside restricted or headless environments.

## v11.3.5 — Performance, Thread-Safety, and Containerization Hardening (May 26, 2026)

### Added

- **Dynamic Text Overlay Cache** — Implemented an efficient memory-managed texture cache for active screen text overlays (including the clock, file paths, and counters). This completely eliminates high-frequency texture allocation cycles and CPU-to-GPU synchronization stalls, drastically lowering CPU usage and ensuring a lockstep, fluid 60 FPS rendering performance.

### Fixed

- **Dashboard Web Remote Concurrency** — Redesigned the remote control dashboard web server to process network client requests asynchronously. Large file previews or slow client connections no longer block the main dashboard loop or disrupt Home Assistant integrations.
- **Docker-Compatible Screen Control** — Standardized display backlight control using standard native Linux graphics interfaces, restoring fully functional and high-performance screen blanking/wake-up states when running inside isolated, privileged containers.
- **Multithreaded Time-Parsing Race Condition** — Fixed a thread-safety race condition on static system time buffers during concurrent background media scans, ensuring metadata and dates are classified accurately without occasional corruption.
- **Contiguous Filename Date Filtering** — Standardized seasonal scan rules to properly parse standard contiguous date naming patterns, ensuring photos labeled in YYYYMMDD formats are correctly filtered into their respective seasonal calendar windows.
- **Startup Integrity Schema Verification** — Updated startup checks to verify recently added device classification metadata columns, preventing runtime query errors on upgraded databases.
- **Improved Logging Resilience** — Hardened system diagnostics to flush stdout streams immediately upon startup even if physical log writing is restricted, and added bounds checking to prevent potential runtime memory overflow anomalies.

### Fixed

- **Network Mount Drop Recovery** — Fixed system instability and abrupt crashes that occurred when the network storage (CIFS/NAS) disconnected or dropped under heavy load. The media scanner and slideshow loop now gracefully survive brief network interruptions without throwing fatal unhandled system exceptions or shutting down the background daemon.

## v11.3.3 — Critical Slideshow Transition Fix (May 26, 2026)

### Fixed

- **Slideshow Transition Crash Loop** — Resolved a critical logic error in the background preloading queue where processing images below the downsampling limit caused the original sharp pixels buffer to be prematurely freed and corrupted. This eliminates random segmentation faults and crash loops during transitions, ensuring the system runs smoothly without briefly showing the boot splash screen.

## v11.3.2 — Unified CLI Management Utility (May 26, 2026)

### Added

- **Simplified Command Line Management (pitrove)** — Introduced a new unified terminal CLI tool (`pitrove`) installed dynamically on the system. Users can now easily manage their digital frame with simple commands: `pitrove config` to launch settings, `pitrove restart` to apply changes, `pitrove logs` to tail rendering diagnostic entries, and `pitrove status` to check background daemon health.

## v11.3.1 — Configurable Logging and Preloader Settings (May 26, 2026)

### Added

- **Dynamic Background Preloading Pool Configuration** — Exposed the background preloading queue capacity (`preload_capacity`, default `4`, range `1` to `32`) and thread pool worker count (`preload_workers`, default `2`, range `1` to `16`) to the user configuration. Users can now fine-tune resource usage based on their system specifications, allowing low-memory boards (like Pi Zero/3) to scale down preloading to save RAM and high-end boards (like Pi 5) to scale up for seamless transitions.
- **Configurable Log File Rotation** — Added support for a configurable log file retention limit (`log_keep_count`, default `5`, range `1` to `100`). Users can now control how many historical log files the system retains before automatically rotating and purging older logs.
- **Terminal UI Wizard Options** — Fully integrated the new preloader capacity, preloader worker pool size, and log keep limit settings into the interactive Terminal UI (TUI) wizard configuration menus. Changes can be edited in real-time and written cleanly back to the system settings file.

## v11.3.0 — Blurred Photo Background, Color-Matched Matte & Edge Glow (May 25, 2026)

### Added

- **Blurred Photo Background** — Enabled by default (`blurred_background = 1`). Each photo is blurred via a fast 3-pass separable box blur and rendered as the full-screen background behind the image, border, and bias lighting, creating a cinematic extended-reach visual effect.
- **Color-Matched Matte Border** — Enabled by default (`color_matched_matte = 1`). The border area surrounding each photo is filled with the image's own center-average color at a configurable opacity, producing a seamless wash that matches the photo's palette.
- **Symmetric 360-Degree Edge Glow** — Redesigned bias lighting to utilize a fully continuous 2D falloff from the image border and a radial gradient from the corners. The edge and corner lighting bounds are mathematically aligned to eliminate seams and gaps, producing a seamless and perfectly unified 360-degree glow frame around the physical borders.
- **Configurable Blur, Matte, Vignette & Glow** — New settings: `blur_radius` (6–24, default 14), `matte_opacity` (0.05–0.50, default 0.20), `vignette_strength` (0.10–0.80, default 0.35), and `glow_depth` (16–120, default 43). All tuned for natural appearance without heavy GPU cost (~5ms on Pi 4/5).

### Fixed

- **TUI Startup Display Freeze** — Resolved a drawing logic issue where the TUI would load into a blank settings screen until a key was pressed. The settings list is now fully drawn and visible immediately upon startup.
- **Midnight Playlist Re-Filtering Race Condition** — Fixed a data race that occurred during midnight playlist re-filtering, eliminating potential system lockups and memory crashes.
- **Mapped Dissolve Transition Effect** — Restored the built-in GPU dissolve transition effect mapping, allowing users to select and play the dissolve scatter effect cleanly.

## v11.2.3 — Slideshow Image Stuck Fix (May 25, 2026)

### Fixed

- **Slideshow stuck on same image** — The transition check incorrectly required twin-portrait data even for single images, causing the next image load to always fail. Single photos now advance correctly.

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

- **CRITICAL · Preload tight-loop CPU exhaustion** — `preload_next()` called `preload_running.store(true)` _before_ thread spawn, then `advance()` reset it to false before the thread started, creating a race where `update()` saw both flags false and forked a new thread every frame. Fixed by setting `preload_running=false` and resetting state atomically before thread spawn, eliminating the race window.

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
- **CRITICAL · Missing overlays — rlBindTexture removed** — `rlBindTexture(0)` was removed in v7.0.3 (compile error on Pi). It IS needed for texture cache sync: mpv unbinds textures, Raylib's cache desyncs, DrawText renders invisible text. _Note: rlBindTexture not available on Pi's Raylib (GLES2) build — texture reset achieved via `glActiveTexture(GL_TEXTURE0)` + `glBindTexture(GL_TEXTURE_2D, 0)` + `rlDisableShader()`_
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
