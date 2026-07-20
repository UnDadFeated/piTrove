#include "error_db.h"

std::vector<ErrorSeed> get_all_error_seeds() {
    return {
        // --- Storage & Network Layers (E100s) ---
        {
            "E101", 
            "NAS_MOUNT_FAILED", 
            "The network storage mount at /app/media is empty, inaccessible, or failed to mount.", 
            "Ensure your NAS is online, credentials in nas.cred are correct, and the systemd automount in /etc/fstab is active."
        },
        {
            "E102", 
            "WIFI_DISCONNECTED", 
            "No active network interfaces are detected, or the target gateway is unreachable.", 
            "Check your Raspberry Pi Wi-Fi settings in NetworkManager, or check physical router power and Ethernet connections."
        },
        {
            "E103", 
            "IP_CONFIGURATION_ERROR", 
            "The system is bound to a self-assigned IP (APIPA) or conflicts with another network node.", 
            "Renew DHCP leases or check for static IP conflicts within your local router configuration panel."
        },
        {
            "E104", 
            "PORT_BIND_CONFLICT", 
            "The HTTP server failed to bind to its target ports due to a port bind conflict.", 
            "Ensure another instance of piTrove is not running, or configure a different http_port in config.toml."
        },
        {
            "E105", 
            "DNS_RESOLUTION_FAILURE", 
            "The system failed to resolve public internet domain names (e.g. googleapis.com).", 
            "Verify that your local router's DNS settings or /etc/resolv.conf contain valid, reachable upstream nameservers (like 8.8.8.8)."
        },
        {
            "E106",
            "ETHERNET_LINK_DOWN",
            "The physical Ethernet link is down or cable is unplugged.",
            "Check the physical RJ45 connection on the Raspberry Pi and switch/router ports."
        },
        {
            "E107",
            "NTP_SYNC_FAILED",
            "System clock is not synchronized via NTP, causing SSL handshake and OAuth failures.",
            "Enable systemd-timesyncd and ensure internet access to pool.ntp.org is available."
        },
        {
            "E108",
            "CIFS_MOUNT_TIMEOUT",
            "The mount request to the Samba/CIFS share timed out.",
            "Check if NAS is awake, check firewall settings, or adjust mount timeout parameters in /etc/fstab."
        },
        {
            "E109",
            "NAS_CREDENTIALS_INVALID",
            "Authentication to the network storage share was rejected.",
            "Verify the username and password in ~/piTrove/config/nas.cred."
        },
        {
            "E110",
            "LOCAL_MEDIA_DIR_READ_ONLY",
            "The local media mount directory is marked read-only on the host.",
            "Run chmod/chown to grant write access or verify mount permissions on the host filesystem."
        },
        {
            "E111",
            "GATEWAY_UNREACHABLE",
            "The default gateway is unreachable via ping.",
            "Check your local router settings, routing tables, and IP subnet configurations."
        },
        {
            "E112",
            "FIREWALL_BLOCK_HTTP",
            "Local firewall rules (iptables/ufw) are blocking the HTTP server port.",
            "Add a firewall rule to allow traffic on the configured http_port (default 9000)."
        },
        {
            "E113",
            "SOCKET_DESCRIPTOR_EXHAUSTION",
            "The application has run out of available file descriptors for network sockets.",
            "Check system limit (ulimit -n) and identify any network socket leaks."
        },
        {
            "E114",
            "SND_TCP_WINDOW_FULL",
            "TCP send buffer is full, indicating network congestion or a slow client.",
            "Optimize network performance, check Wi-Fi signal strength, or replace old network cabling."
        },
        {
            "E115",
            "DNS_RECURSION_DISABLED",
            "Configured DNS server refused recursive queries.",
            "Configure standard recursive nameservers like 1.1.1.1 or 8.8.8.8 in network settings."
        },
        {
            "E116",
            "WIFI_SIGNAL_WEAK",
            "The active Wi-Fi connection signal strength is below acceptable threshold (-80dBm).",
            "Move the Raspberry Pi closer to the router or install a high-gain external antenna."
        },
        {
            "E117",
            "IPV6_ADDRESS_CONFLICT",
            "An IPv6 duplicate address detection (DAD) conflict was detected on the link.",
            "Check for stateless address auto-configuration (SLAAC) or DHCPv6 lease conflicts."
        },
        {
            "E118",
            "SST_TUNNEL_DISCONNECTED",
            "The secure SSH remote management tunnel has disconnected.",
            "Verify sshpass options, check remote host sshd configurations, and ensure network path is stable."
        },
        {
            "E119",
            "ROUTER_DHCP_EXHAUSTION",
            "The local router failed to assign an IP address due to DHCP pool exhaustion.",
            "Expand the DHCP pool size on your router or shorten lease duration settings."
        },
        {
            "E120",
            "SMB_PROTOCOL_MISMATCH",
            "The NAS storage share requires a newer or older version of SMB protocol.",
            "Explicitly specify vers=3.0 or vers=2.0 in your CIFS mount options."
        },
        {
            "E121",
            "NETWORK_BANDWIDTH_LOW",
            "Available network bandwidth to media server is insufficient for smooth playback.",
            "Check if others are saturated on network, or switch from Wi-Fi to Gigabit Ethernet."
        },
        {
            "E122",
            "PORT_SCAN_ATTACK_DETECTED",
            "A high rate of connection attempts has been detected on the dashboard port.",
            "Implement fail2ban or change http_port to a non-standard port."
        },
        {
            "E123",
            "SSH_KEY_EXPIRED",
            "The configured SSH key used for background synchronization has expired or is invalid.",
            "Regenerate SSH keys using ssh-keygen and authorize them on the remote destination."
        },
        {
            "E124",
            "HTTP_PROXY_UNREACHABLE",
            "A configured HTTP proxy server is unreachable or rejecting connections.",
            "Verify the proxy settings in the environment or unset HTTP_PROXY variables."
        },
        {
            "E125",
            "NAS_OFFLINE_STANDBY",
            "The NAS is in a low-power sleep mode and did not respond within the spin-up timeout.",
            "Enable wake-on-LAN (WOL) or increase client-side connection timeout parameters."
        },
        {
            "E126",
            "HTTP_LOG_STREAM_IO_ERROR",
            "HTTP server failed to stream system logs because the log file could not be read or does not exist.",
            "Verify that the log directory exists, is writeable, and g_logger has successfully initialized."
        },

        // --- Media Loader & Rendering Layers (E200s) ---
        {
            "E201", 
            "IMAGE_LOAD_ERROR", 
            "The media loader encountered a fatal error while trying to decode an image.", 
            "Verify that the file is not corrupted and its image format is fully supported by SDL3/stb."
        },
        {
            "E202", 
            "VIDEO_PLAYER_CRASH", 
            "The video decoder exited abnormally with a critical playback failure.", 
            "Check if the video encoding is supported (HEVC/H.264), or if subtitle files are malformed/incomplete."
        },
        {
            "E203", 
            "INVALID_ASPECT_RATIO", 
            "The loaded image has extreme dimensions (e.g., width or height close to zero) that prevent mathematical layout calculations.", 
            "Remove the corrupted image file or check standard camera aspect ratio settings."
        },
        {
            "E204", 
            "MISSING_FONT_FILES", 
            "The font rendering subsystem failed to locate required TrueType font files.", 
            "Verify that /app/src/fonts/DejaVuSansMono-Bold.ttf exists, or specify a valid font_path in config.toml."
        },
        {
            "E205", 
            "AUDIO_DEVICE_ERROR", 
            "The SDL3 audio subsystem failed to initialize the configured audio output device.", 
            "Verify your audio output device name in config.toml, or verify your Raspberry Pi headphone jack/HDMI audio routes."
        },
        {
            "E206",
            "EXIF_PARSING_FAILED",
            "The image loader failed to read EXIF orientation metadata.",
            "Check if the image file has a corrupted EXIF header or strip bad metadata."
        },
        {
            "E207",
            "UNSUPPORTED_COLOR_SPACE",
            "The image uses an unsupported color space (e.g. CMYK or 16-bit per channel).",
            "Convert the image to 8-bit RGB/RGBA using an image editor or ImageMagick."
        },
        {
            "E208",
            "HEVC_DECODE_FAILED",
            "The hardware decoder failed to decode the HEVC/H.265 video stream.",
            "Check if GPU memory (gpu_mem) is sufficient, or fall back to software decoding."
        },
        {
            "E209",
            "FFMPEG_PROBE_ERROR",
            "FFprobe failed to analyze the media file to extract duration or codec details.",
            "Verify that the file is not empty and is a valid video container."
        },
        {
            "E210",
            "SUBTITLE_FORMAT_INVALID",
            "The SRT/VTT subtitle file has formatting errors or bad timestamps.",
            "Validate the subtitle file with an editor and ensure UTF-8 encoding."
        },
        {
            "E211",
            "TWIN_PORTRAIT_MISMATCH",
            "Could not find a matching portrait companion image for twin layout within the time limit.",
            "Add more portrait aspect ratio photos to your media folder to enable twin rendering."
        },
        {
            "E212",
            "LAYOUT_BOUNDS_OVERFLOW",
            "The text overlay is too long and exceeds the screen display margins.",
            "Shorten the filename, caption, or description, or reduce font size in config."
        },
        {
            "E213",
            "stbi_load_OUT_OF_MEMORY",
            "The image decoder ran out of memory while decompressing a large image.",
            "Reduce the resolution of the image; avoid photos larger than 50 megapixels."
        },
        {
            "E214",
            "SDL_SURFACE_CREATION_FAILED",
            "Failed to create an SDL_Surface container for decoded pixel data.",
            "Verify that system memory is not exhausted and that SDL3 library is properly initialized."
        },
        {
            "E215",
            "GLYPH_RENDER_FAILED",
            "Failed to render a specific text character glyph.",
            "Ensure the selected font supports the required unicode block or character set."
        },
        {
            "E216",
            "VIGNETTE_SHADER_ERROR",
            "GPU shader compiler failed to assemble the vignette vignette-bias lighting shader.",
            "Check OpenGL/EGL driver status and ensure Mesa library dependencies are up to date."
        },
        {
            "E217",
            "TRANSITION_DISSOLVE_FAILED",
            "The GPU-based cross-dissolve transition failed to compile or run.",
            "Verify EGL context is valid and that system is not dropping frame rendering calls."
        },
        {
            "E218",
            "MEDIA_QUEUE_EMPTY",
            "The slideshow queue is empty because no playable images or videos match filters.",
            "Add media files to scanned directories or adjust date/month spread parameters."
        },
        {
            "E219",
            "FPS_DROP_THRESHOLD_EXCEEDED",
            "The rendering pipeline dropped below target frames per second (FPS).",
            "Close other resource-heavy background processes or reduce transitions complexity."
        },
        {
            "E220",
            "CORRUPT_PNG_CHUNKS",
            "PNG file has critical chunk errors (e.g. invalid CRC checksums).",
            "Repair the corrupted image or re-export it from your photo library."
        },
        {
            "E221",
            "GIF_DECODE_TIMEOUT",
            "An animated GIF failed to decode within the allocated time limit.",
            "Avoid excessively long or high-frame-rate animated GIFs in slideshow."
        },
        {
            "E222",
            "COLOR_PROFILE_CORRUPT",
            "The ICC color profile embedded in the image is corrupted or invalid.",
            "Strip the embedded ICC profile or re-save the image with a standard sRGB profile."
        },
        {
            "E223",
            "VIDEO_SEEK_ERROR",
            "The video player encountered a seek failure during media start.",
            "Ensure the video index is fully indexed, or re-encode with keyframe intervals."
        },
        {
            "E224",
            "BLACK_FRAME_DETECTED",
            "The rendered output is fully black, suggesting a rendering pipeline failure.",
            "Check EGL window binding and verify display connection status."
        },
        {
            "E225",
            "FONT_SIZE_CLAMPED",
            "The configured font size is extremely large or small and was clamped.",
            "Adjust the font size parameters in config.toml to match standard values (12-72pt)."
        },
        {
            "E226",
            "VIDEO_FORK_FAILED",
            "The system failed to spawn the video player subprocess due to a process creation error.",
            "Check system process limits (ulimit -u) and available memory. Restart the container if the issue persists."
        },

        // --- Cloud Integration Layers (E300s) ---
        {
            "E301", 
            "GOOGLE_PHOTOS_SYNC_FAILED", 
            "The Google Photos synchronizer failed to authenticate or sync cloud media.", 
            "Verify your internet connection and ensure your OAuth Client ID/Secret and Refresh Token are correct."
        },
        {
            "E302", 
            "GOOGLE_PHOTOS_RATE_LIMITED", 
            "Google Photos API requests have exceeded quota limits (HTTP 429).", 
            "Increase your sync_interval_mins in config.toml to avoid frequent API calls, or check your developer console."
        },
        {
            "E303", 
            "GOOGLE_PHOTOS_CLIENT_INVALID", 
            "Google Photos Client ID or Client Secret has been rejected by Google OAuth server.", 
            "Reconfigure credentials at /google_photos_setup, ensuring the keys matched your GCP OAuth desktop client configuration."
        },
        {
            "E304",
            "GOOGLE_PHOTOS_REFRESH_TOKEN_EXPIRED",
            "The OAuth refresh token has expired or has been revoked.",
            "Run the Google Photos Setup Wizard again via the dashboard to authorize."
        },
        {
            "E305",
            "GOOGLE_PHOTOS_ALBUM_NOT_FOUND",
            "The configured Album ID does not exist or is inaccessible.",
            "Verify the Album ID in your config or ensure the album is shared/public."
        },
        {
            "E306",
            "GOOGLE_PHOTOS_NETWORK_TIMEOUT",
            "Connection timed out while communicating with Google API servers.",
            "Check local network latency, DNS response times, or firewall outbound rules."
        },
        {
            "E307",
            "GOOGLE_PHOTOS_PAYLOAD_INVALID",
            "Google API returned an invalid or corrupted JSON payload.",
            "Check API status page or wait for a subsequent scheduled synchronization sweep."
        },
        {
            "E308",
            "GOOGLE_PHOTOS_MEDIA_URL_EXPIRED",
            "A cloud media download URL expired before download was completed.",
            "Ensure sync sweeps are fast and network speed is sufficient to fetch images."
        },
        {
            "E309",
            "GOOGLE_PHOTOS_HTTP_500",
            "Google Photos server encountered an internal server error (HTTP 500/503).",
            "This is an upstream Google server issue. The app will retry on the next interval."
        },
        {
            "E310",
            "GOOGLE_PHOTOS_UNAUTHORIZED_SCOPE",
            "The OAuth token does not grant required scopes to read library data.",
            "Make sure to check all permissions during the Google consent screen workflow."
        },
        {
            "E311",
            "GOOGLE_PHOTOS_DISK_FULL",
            "Failed to cache cloud media because the destination drive is completely full.",
            "Free up disk space on the host, or expand system storage capacity."
        },
        {
            "E312",
            "GOOGLE_PHOTOS_LOCK_ACQUIRE_FAIL",
            "Failed to acquire sync lock; another Google Photos sync instance is active.",
            "Wait for the active sync thread to finish or restart the container."
        },
        {
            "E313",
            "GOOGLE_PHOTOS_TLS_HANDSHAKE_FAIL",
            "SSL/TLS handshake failed when connecting to googleapis.com.",
            "Ensure the Pi system clock is correct and CA certificate packages are updated."
        },
        {
            "E314",
            "GOOGLE_PHOTOS_API_DISABLED",
            "Google Photos Library API is not enabled in your GCP project.",
            "Go to Google Cloud Console, select your project, and enable 'Photos Library API'."
        },
        {
            "E315",
            "GOOGLE_PHOTOS_REDIRECT_URI_MISMATCH",
            "The redirect URI specified in OAuth configuration mismatch dashboard.",
            "Ensure GCP credentials match the exact host URL used in /google_photos_setup."
        },
        {
            "E316",
            "GOOGLE_PHOTOS_USER_CANCELLED",
            "The user cancelled or rejected permissions on the OAuth consent screen.",
            "Restart the setup flow and click 'Allow' to grant required access."
        },
        {
            "E317",
            "GOOGLE_PHOTOS_DOWNLOAD_CORRUPTED",
            "Downloaded cloud image file size mismatch or checksum failure.",
            "The cache entry was removed and will be automatically redownloaded."
        },
        {
            "E318",
            "GOOGLE_PHOTOS_CONFIG_MISSING",
            "Google Photos is enabled, but required credential fields are blank.",
            "Run the Google Photos Setup Wizard via the dashboard to save configurations."
        },

        // --- Database & Local Storage Layers (E400s) ---
        {
            "E401", 
            "SQLITE_DB_CORRUPTED", 
            "The SQLite cache database encountered a disk I/O failure or structural corruption.", 
            "Ensure the filesystem is not full and write permissions are correct, or delete cache.db to rebuild."
        },
        {
            "E402", 
            "SQLITE_LOCK_TIMEOUT", 
            "The SQLite cache database remains locked by another concurrent process, resulting in a transaction timeout.", 
            "Restart the container or kill conflicting SQLite read/write clients to release transaction locks."
        },
        {
            "E403", 
            "DISK_SPACE_CRITICAL", 
            "Available storage space on the Raspberry Pi host is below critical thresholds (<50MB).", 
            "Prune unused Docker images, logs, or cache files to free storage space on your Raspberry Pi host."
        },
        {
            "E404",
            "SQLITE_CONSTRAINT_VIOLATION",
            "SQLite database statement failed due to a constraint violation.",
            "Verify data schema integrity or delete cache.db to run structural migrations."
        },
        {
            "E405",
            "SQLITE_READONLY_DATABASE",
            "The SQLite database file is marked read-only or filesystem is write-protected.",
            "Check cache.db file ownership (should be pi/root) and write permissions."
        },
        {
            "E406",
            "SQLITE_JOURNAL_MODE_ERROR",
            "Failed to set Write-Ahead Logging (WAL) journal mode on the cache.",
            "Ensure the database file is located on a local drive supporting lock calls."
        },
        {
            "E407",
            "SQLITE_MIGRATION_FAILED",
            "A database schema upgrade migration sequence failed to execute.",
            "Delete the outdated /app/cache/cache.db file to allow fresh initialization."
        },
        {
            "E408",
            "DISK_WRITE_FAIL",
            "Low-level system write operation returned an I/O error.",
            "Check for SD card degradation, physical bad sectors, or loose drive connections."
        },
        {
            "E409",
            "SQLITE_BUSY_RETRY_LIMIT",
            "Database busy retry limit reached; transaction abandoned.",
            "Reduce execution of competing sqlite clients or optimize query indexing."
        },
        {
            "E410",
            "SQLITE_PREPARE_STMT_FAIL",
            "Failed to prepare a SQL database query statement.",
            "Verify the database schema matches compiled code expectations."
        },
        {
            "E411",
            "CACHE_DIRECTORY_NOT_FOUND",
            "The configured cache folder `/app/cache` does not exist.",
            "Check Docker volume mount binds or recreate the directory manually."
        },
        {
            "E412",
            "SQLITE_COLUMN_MISMATCH",
            "The database row mapping failed because table columns mismatch schema.",
            "Rebuild cache.db database by deleting the old file and restarting."
        },
        {
            "E413",
            "SQLITE_OPEN_FAILED",
            "Failed to open cache database connection handle.",
            "Ensure the container mount has correct user group read/write privileges."
        },
        {
            "E414",
            "DISK_RESERVED_BLOCKS_ONLY",
            "Only superuser reserved blocks remain on the media storage drive.",
            "Clean up unused software packages or logs to release disk space."
        },
        {
            "E415",
            "TEMP_FILESYSTEM_FULL",
            "The system temporary filesystem (/tmp) is full, blocking process execution.",
            "Delete stale temporary file handles or resize your tmpfs partition."
        },
        {
            "E416",
            "CACHE_CLEANUP_FAILED",
            "Failed to prune old cache entries due to filesystem lock or permission.",
            "Verify container is running with proper privileges and directory is writeable."
        },
        {
            "E417",
            "DATABASE_SYNC_TIMEOUT",
            "SQLite synchronization call did not return within expected interval.",
            "Check for heavy disk I/O bottlenecks or slow storage medium."
        },
        {
            "E418",
            "SQLITE_BLOB_READ_FAILED",
            "Failed to load cached thumbnail BLOB data.",
            "Prune bad items from cache.db or trigger a full database recache."
        },

        // --- Hardware & Operating System Layers (E500s) ---
        {
            "E501", 
            "SYSTEM_OVERHEATING", 
            "The system SoC temperature has crossed critical thresholds, triggering thermal throttling.", 
            "Ensure your Raspberry Pi enclosure has adequate ventilation or active cooling (fan/heatsink)."
        },
        {
            "E502", 
            "MEMORY_ALLOCATION_FAILED", 
            "The system is out of memory and failed to allocate memory buffers for rendering.", 
            "Check for memory leaks or other heavy concurrent services running on your Raspberry Pi host."
        },
        {
            "E503",
            "SYSTEM_UNDERVOLTAGE_WARNING",
            "The Raspberry Pi power supply is delivering less than 4.63V (Undervoltage).",
            "Replace the power supply with an official 5V/5.1V Raspberry Pi power adapter."
        },
        {
            "E504",
            "CPU_THERMAL_THROTTLING",
            "The CPU frequency has been capped to protect hardware from overheating.",
            "Install active cooling or heatsink to prevent SoC thermal throttling."
        },
        {
            "E505",
            "WATCHDOG_TIMEOUT_ALARM",
            "The system hardware watchdog timer expired without a heartbeat signal.",
            "Check for long-running blocking operations in the main rendering thread."
        },
        {
            "E506",
            "GPU_OUT_OF_MEMORY",
            "The GPU has run out of VRAM for textures, shaders, or buffers.",
            "Increase gpu_mem settings in /boot/firmware/config.txt or reduce resolution."
        },
        {
            "E507",
            "UDEV_RULES_MISSING",
            "Udev rules are missing, preventing non-root access to DRM devices.",
            "Install appropriate 99-pitrove.rules to grant DRM access permissions."
        },
        {
            "E508",
            "GPIO_INITIALIZATION_ERROR",
            "Failed to initialize standard GPIO bus interface.",
            "Ensure your container has privileges or access to /dev/gpiomem."
        },
        {
            "E509",
            "SYSTEM_RTC_LOSS",
            "The Real-Time Clock (RTC) module has lost power or is not responding.",
            "Replace the CR1220 battery on the RTC board or configure network time."
        },
        {
            "E510",
            "KERNEL_PANIC_DETECTED",
            "A critical kernel warning or hardware failure check was detected.",
            "Check system logs (dmesg) for hardware issues or SD card failure."
        },
        {
            "E511",
            "PWM_AUDIO_CONFLICT",
            "Analog audio output conflicts with configured PWM GPIO signals.",
            "Disable onboard audio in /boot/firmware/config.txt or reassign GPIO pins."
        },
        {
            "E512",
            "USB_OVERCURRENT_LIMIT",
            "A connected USB device exceeded safe current limits.",
            "Disconnect power-hungry external hard drives or use a powered USB hub."
        },
        {
            "E513",
            "SWAP_SPACE_EXHAUSTED",
            "Virtual swap memory is fully exhausted.",
            "Increase swap file size in /etc/dphys-swapfile or optimize memory usage."
        },
        {
            "E514",
            "CPU_CORE_STUCK",
            "A worker thread is stuck on a CPU core without responding.",
            "Debug thread deadlocks or thread synchronization primitives."
        },
        {
            "E515",
            "DRM_DRI_DEVICE_NOT_FOUND",
            "DRM/DRI DRI card device node (/dev/dri/card*) is missing.",
            "Ensure graphics drivers (vc4-fkms-v3d or vc4-kms-v3d) are enabled."
        },
        {
            "E516",
            "D_BUS_COMMUNICATION_FAIL",
            "Unable to communicate with host systemd via D-Bus interface.",
            "Check bind mounts for /var/run/dbus/system_bus_socket in docker-compose.yml."
        },
        {
            "E517",
            "HARDWARE_DECODER_MISSING",
            "No compatible hardware video decoding blocks (V4L2/MMAL) were detected.",
            "Verify driver configuration and package versions in the OS."
        },
        {
            "E518",
            "FIRMWARE_OUTDATED",
            "The Raspberry Pi firmware is outdated and could cause EGL/DRM bugs.",
            "Run sudo rpi-update on the host to update system firmware."
        },
        {
            "E519",
            "NETWORK_RECOVERY_REBOOT",
            "The system has been disconnected from the network for over 3 minutes and is initiating an automatic recovery reboot.",
            "Check your router, Wi-Fi adapter, and network cables. Ensure the device is within range of the access point."
        },
        {
            "E520",
            "NETWORK_RECOVERY_FAILED",
            "The system was unable to re-associate with the access point or reset the network interface during recovery.",
            "Inspect systemd-networkd, NetworkManager, or wpa_supplicant configurations and logs on the host."
        },
        {
            "E521",
            "INTERFACE_UP_FAILED",
            "The network interface could not be set to UP state after a reset.",
            "Check physical hardware connection of your Wi-Fi interface or RFkill block status."
        },

        // --- Graphic Pipeline & Framebuffer Layers (E600s) ---
        {
            "E525",
            "VIDEO_AUDIO_SWR_FAIL",
            "Failed to initialize FFmpeg audio resampler for video audio stream.",
            "Verify video audio format; system will continue video playback without audio."
        },
        {
            "E526",
            "VIDEO_HW_FALLBACK_WARN",
            "Hardware video decoder (V4L2 M2M) configuration failed; falling back to software decoding.",
            "No action required; software decoding will handle the video."
        },
        {
            "E601", 
            "DRM_CONNECTOR_MISSING", 
            "The modesetting subsystem failed to discover any connected HDMI display ports.", 
            "Ensure HDMI cables are securely plugged into the Raspberry Pi's HDMI0 port and the display panel is powered on."
        },
        {
            "E602", 
            "EGL_PAGE_FLIP_FAILED", 
            "The EGL window manager failed to swap screen buffers, indicating a DRM page flip timeout.", 
            "Restart the graphics pipeline or check for card modesetting driver issues."
        },
        {
            "E603", 
            "SDL_WINDOW_CREATION_FAILED", 
            "The SDL3 graphics subsystem failed to instantiate a hardware-accelerated full-screen rendering window.", 
            "Ensure your user is in the 'video' group and has direct hardware access to /dev/dri/card* nodes."
        },
        {
            "E604",
            "DRM_PLANE_ALLOCATION_FAILED",
            "DRM system failed to allocate an overlay plane for rendering.",
            "Reduce layers, adjust resolution, or restart the rendering pipeline."
        },
        {
            "E605",
            "CURSOR_CREATION_FAILED",
            "Failed to instantiate EGL cursor texture resources.",
            "Ensure mouse cursor is disabled in options or check graphics memory."
        },
        {
            "E606",
            "EGL_CONTEXT_SHARING_ERROR",
            "Failed to share EGL context across loading threads.",
            "Ensure display drivers support concurrent thread context bindings."
        },
        {
            "E607",
            "REFRESH_RATE_MISMATCH",
            "Connected display refresh rate is invalid or unsupported.",
            "Explicitly specify target refresh rate or monitor display EDID data."
        },
        {
            "E608",
            "DRM_MASTER_LOCK_LOST",
            "The application lost DRM master privileges to another process.",
            "Stop conflicting display managers (like lightdm/gdm) on the host."
        },
        {
            "E609",
            "EGL_INITIALIZATION_ERROR",
            "Failed to initialize EGL rendering environment.",
            "Check your graphics configuration and mesa-egl packages version."
        },
        {
            "E610",
            "SDL_RENDERER_CREATE_FAIL",
            "SDL3 failed to instantiate a hardware-accelerated renderer.",
            "Verify EGL environment variables and check display connectivity."
        },
        {
            "E611",
            "DRM_MODE_SET_FAIL",
            "Failed to set desired screen resolution mode on DRM connector.",
            "Verify display supports the configured screen width and height."
        },
        {
            "E612",
            "GBM_DEVICE_CREATE_FAIL",
            "Failed to initialize Generic Buffer Manager (GBM) device.",
            "Ensure you have read/write access to DRM card nodes in container."
        },
        {
            "E613",
            "EGL_CONFIG_SELECT_FAIL",
            "No compatible EGL frame buffer configurations were discovered.",
            "Check for correct pixel format and depth buffer support in driver."
        },
        {
            "E614",
            "DOUBLE_BUFFERING_UNAVAILABLE",
            "The hardware rendering pipeline does not support double-buffering.",
            "Update graphics driver configuration or reduce page-flip expectations."
        },
        {
            "E615",
            "SCREEN_BLANK_TIMEOUT",
            "Failed to programmatically wake or blank the display via DRM/KMS.",
            "Verify KMS power-management settings or display hardware standby state."
        },
        {
            "E616",
            "EGL_SURFACE_CREATE_FAIL",
            "Failed to construct EGL window surface using GBM.",
            "Verify EGL window configuration parameters match display capabilities."
        },
        {
            "E617",
            "DRM_CRTC_ACQUIRE_FAIL",
            "Failed to acquire CRTC display controller interface.",
            "Check if another graphics application is holding the display resource."
        },
        {
            "E618",
            "FRAMEBUFFER_MAP_ERROR",
            "Failed to map physical memory buffer for EGL frames.",
            "Check for physical VRAM exhaustion or system memory constraints."
        },
        {
            "E619",
            "TOUCHSCREEN_DEVICE_NOT_FOUND",
            "Touchscreen enabled in configuration, but no active input device with touch capabilities was detected.",
            "Check if the touchscreen USB/DSI cable is connected, or ensure your user has permissions to access /dev/input/event*."
        },

        // --- Smart Home & Integration Layers (E700s) ---
        {
            "E701", 
            "MQTT_BROKER_UNREACHABLE", 
            "The smart home subsystem failed to establish a connection to the MQTT broker.", 
            "Ensure the MQTT broker is online, and its IP address and port are correctly configured in config.toml."
        },
        {
            "E702", 
            "MQTT_SUBSCRIPTION_FAILED", 
            "The MQTT client connected to the broker but failed to subscribe to the motion sensor topic.", 
            "Check broker ACL permissions, or verify the topic names configured in config.toml."
        },
        {
            "E703",
            "MQTT_PAYLOAD_PARSE_ERROR",
            "Received an unparseable or corrupted payload on MQTT topic.",
            "Ensure the smart home triggers send clean, supported JSON or text data."
        },
        {
            "E704",
            "MQTT_JSON_FORMAT_INVALID",
            "MQTT JSON payload is missing mandatory fields (e.g. state/status).",
            "Adjust your Home Assistant automations or MQTT publishing script format."
        },
        {
            "E705",
            "MQTT_HEARTBEAT_TIMEOUT",
            "Smart home sensor heartbeat signals have stopped arriving.",
            "Check sensor battery levels, wireless range, or physical power status."
        },
        {
            "E706",
            "HA_REGISTRATION_FAILED",
            "Failed to register automatic discovery entity in Home Assistant.",
            "Ensure Home Assistant discovery is enabled on the broker and config."
        },
        {
            "E707",
            "MQTT_SSL_HANDSHAKE_FAIL",
            "SSL/TLS handshake with secure MQTT broker failed.",
            "Check certificate verification paths and CA certificates on the Pi."
        },
        {
            "E708",
            "MQTT_AUTH_REJECTED",
            "MQTT broker rejected username or password credentials.",
            "Verify MQTT client credentials configured inside config.toml."
        },
        {
            "E709",
            "MQTT_KEEPALIVE_MISSED",
            "Client missed keepalive ping window, forcing broker disconnection.",
            "Check network routing latency, processor spikes, or network drops."
        },
        {
            "E710",
            "MQTT_QOS_UNSUPPORTED",
            "Requested MQTT Quality of Service (QoS) level is unsupported by broker.",
            "Set mqtt_qos to 0 or 1 in config.toml to match broker capacities."
        },
        {
            "E711",
            "MQTT_TOPIC_RESERVED",
            "Configured MQTT topic uses reserved system prefixes or paths.",
            "Choose a custom, unique topic name that does not conflict with broker systems."
        },
        {
            "E712",
            "MQTT_CLIENT_ID_COLLISION",
            "MQTT connection rejected because Client ID is already in use.",
            "Verify that another piTrove instance or device is not using the same client ID."
        },
        {
            "E713",
            "MQTT_BUFFER_OVERFLOW",
            "MQTT outbound message queue is full, discarding updates.",
            "Improve network connectivity to broker or increase client queue capacity."
        },
        {
            "E714",
            "MQTT_SOCKET_WRITE_FAIL",
            "Failed to write data packet to MQTT network socket.",
            "Check if MQTT broker crashed or if network routing table updated."
        },
        {
            "E715",
            "MQTT_BRIDGE_DISCONNECTED",
            "An external MQTT bridge has disconnected, severing the path.",
            "Verify bridge configurations on your main smart home hub."
        },
        {
            "E716",
            "MQTT_WILL_MESSAGE_FAIL",
            "Failed to register Last Will and Testament message on connection.",
            "Verify will topic and payload configuration format in config."
        },
        {
            "E717",
            "MQTT_DNS_LOOKUP_FAIL",
            "Failed to resolve MQTT broker hostname.",
            "Ensure DNS services are active or use a static IP for your broker."
        },
        {
            "E718",
            "MQTT_RECONNECT_EXCEEDED",
            "Exceeded maximum automated MQTT reconnection retries.",
            "Wait for the auto-reconnection cooldown or restart the service."
        },

        // --- Config & Application Lifecycle Layers (E800s) ---
        {
            "E801",
            "TOML_PARSE_FAILURE",
            "The configuration file config.toml contains syntax errors.",
            "Validate config.toml with a TOML validator and fix bracket/quote errors."
        },
        {
            "E802",
            "CONFIG_SECTION_MISSING",
            "A mandatory section (e.g. [renderer] or [media]) is missing from config.toml.",
            "Restore the default template config.toml or run the configuration wizard."
        },
        {
            "E803",
            "LOCKFILE_CREATION_FAILED",
            "Failed to create PID lockfile /app/piTrove.lock.",
            "Check write permissions in /app directory and make sure no other instance is running."
        },
        {
            "E804",
            "CMD_ARGUMENTS_INVALID",
            "Supplied command line arguments are invalid or conflicting.",
            "Run piTrove --help to see the list of valid runtime execution flags."
        },
        {
            "E805",
            "THREAD_POOL_STALL",
            "All threads in background preloading pool are blocked or deadlocked.",
            "Reduce heavy concurrent disk/network requests or restart the application."
        },
        {
            "E806",
            "APP_SHUTDOWN_HANG",
            "The application did not shut down cleanly within the timeout window.",
            "A thread failed to respond to shutdown signals; force restart container."
        },
        {
            "E807",
            "HTTP_SETTINGS_CLAMP_VIOLATION",
            "A settings update request from the HTTP dashboard was rejected because values violated safety boundaries.",
            "Review the boundary values in config.cpp/wizard and ensure submitted values fall within allowed limits."
        },
        {
            "E808",
            "SEASONAL_WINDOW_FALLBACK",
            "No date prefixes (YYYY-MM-DD_) detected on folders or filenames.",
            "Fallback to file creation/modification dates is active."
        },
        {
            "E809",
            "WATCHDOG_FORCED_RESTART",
            "The rendering loop froze and the internal watchdog forced an emergency restart.",
            "Check for GPU/EGL driver issues or extremely large image files that stall decoding."
        },
        {
            "E901",
            "HTTP_API_KEY_MISSING",
            "The HTTP dashboard API key is configured but the request was missing an Authorization header.",
            "Include an Authorization: Bearer header with your API key when calling /api/settings/update."
        },
        {
            "E902",
            "HTTP_API_KEY_INVALID",
            "The HTTP dashboard API key provided does not match the configured value.",
            "Verify the API key in config.toml under [remote] api_key matches the one sent in the Authorization header."
        },

        // --- Dashboard Security & Authentication (E900s) ---
        {
            "E903",
            "DASHBOARD_PIN_LOCKOUT",
            "Dashboard access locked after 5 consecutive failed PIN attempts.",
            "Wait 15 seconds for auto-unlock, or restart the container to reset the lockout counter."
        },
        {
            "E904",
            "DASHBOARD_PIN_INVALID",
            "The entered 4-digit PIN does not match the stored dashboard PIN.",
            "Enter the correct dashboard PIN. Default is 0000 if no custom PIN has been set."
        },
        {
            "E905",
            "DASHBOARD_PIN_LOCKOUT_TIMER",
            "Dashboard remains locked due to an active PIN lockout timer.",
            "Wait for the 15-second cooldown to expire. The dashboard will auto-unlock when the timer completes."
        },
        {
            "E906",
            "DASHBOARD_PIN_STORAGE_MISSING",
            "No dashboard PIN is configured in local storage.",
            "Use the default PIN (0000) or change it via the pitrove config command."
        },
        {
            "E907",
            "DASHBOARD_AUTH_REQUIRED",
            "The dashboard settings endpoint requires API key authentication but none was provided.",
            "Set the api_key in config.toml under [remote] and include it as a Bearer token in the Authorization header."
        },
        {
            "E908",
            "DASHBOARD_AUTH_MISMATCH",
            "The API key sent with the request does not match the configured value.",
            "Verify the Bearer token matches the api_key defined in [remote] of config.toml."
        },

        // --- I/O & Health Monitoring (E909-E910) ---
        {
            "E909",
            "HEALTHCHECK_DEBOUNCE",
            "Media directory health checks are cached (5s TTL) to reduce CIFS session load.",
            "No action required. This indicates the I/O throttle is active."
        },

        {
            "E910",
            "IO_BURST_WARNING",
            "Detected rapid successive health check failures within the cache window.",
            "CIFS mount may be unstable. Check network connectivity to NAS."
        }
    };
}
