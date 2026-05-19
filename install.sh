#!/usr/bin/env bash
# install.sh — PiTrove v7.0.0 installer
# Target: Debian Trixie (13) 64-bit on Raspberry Pi 5
# Features: JPEG/TIFF/PNG/WebP/HEIC robust loaders, CRT UI, multi-format support

# Changed -euo to -eo to prevent crashes on unbound vars during fresh installs
set -eo pipefail

# ── Helpers ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[INFO]${NC}   $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}   $*"; }
fail()  { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
yesno() {
    echo -n "$* [y/N]: "
    read -r resp < /dev/tty
    [[ "$resp" == [yY]* ]]
}

# ── Pre-flight checks ───────────────────────────────────────────────────────
echo "============================================"
  echo "  PiTrove v7.0.0 Installer"
echo "  Target: Raspberry Pi 5 / ARM64"
echo "============================================"
echo

# 1. Detect primary user (fallback if UID 1000 is modified)
PRIMARY_USER=$(getent passwd 1000 | cut -d: -f1 || true)
if [[ -z "$PRIMARY_USER" ]]; then
    PRIMARY_USER=$(logname 2>/dev/null || echo $USER)
fi

if [[ "$PRIMARY_USER" == "root" ]]; then
    fail "Installer detected 'root'. Please run from a standard user account with sudo."
fi
PRIMARY_HOME="/home/$PRIMARY_USER"
info "Primary user: $PRIMARY_USER ($PRIMARY_HOME)"

# 2. Bootstrap packages (git, lsb_release, pkg-config, curl needed below)
info "Installing bootstrap packages..."
apt-get update -qq || fail "apt-get update failed"
apt-get install -y -qq git curl lsb-release pkg-config || fail "Bootstrap install failed"
ok "Bootstrap packages ready"

# 3. OS check
if [[ "$(lsb_release -si)" != "Debian" ]]; then
    fail "This installer requires Debian Trixie 64-bit"
fi
if [[ "$(lsb_release -rs)" != "13" ]]; then
    fail "This installer requires Debian Trixie (13), found $(lsb_release -rs)"
fi
if [[ "$(uname -m)" != "aarch64" ]]; then
    fail "This installer requires ARM64 (aarch64), found $(uname -m)"
fi
ok "Debian Trixie 64-bit confirmed"

# 4. Root check
if [[ "$(id -u)" -ne 0 ]]; then
    fail "This script must be run as root (sudo)"
fi

# 5. Kill any conflicting display servers (NEVER disable ssh)
info "Checking for conflicting display servers..."
for svc in labwc-tty1 seatd; do
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
        systemctl stop "$svc"
        systemctl disable "$svc"
        info "Stopped and disabled $svc"
    fi
done

# 5b. Ensure SSH is still alive — critical safeguard
if ! systemctl is-active --quiet ssh 2>/dev/null; then
    warn "SSH service is not active, attempting to start..."
    apt-get install -y -qq openssh-server 2>/dev/null || true
    systemctl enable ssh 2>/dev/null || true
    systemctl start ssh 2>/dev/null || true
    if systemctl is-active --quiet ssh 2>/dev/null; then
        info "SSH service restored"
    else
        warn "WARNING: Could not start SSH service — reboot and check manually"
    fi
fi

# 6. Space check
AVAIL=$(df --output=avail / | tail -n 1 | tr -d ' ')
if [[ "$AVAIL" -lt 5242880 ]]; then
    warn "Only ${AVAIL}KB free on / (need 5GB for build + SQLite cache)"
fi
ok "Disk space OK: ${AVAIL}KB available"

# ── Install packages ─────────────────────────────────────────────────────────
info "Installing dependencies (this may take a few minutes)..."

apt-get install -y -qq \
    build-essential cmake git curl pkg-config \
    libsqlite3-dev libexif-dev libjpeg-dev libpng-dev libtiff-dev libheif-dev libwebp-dev \
    libjpeg62-turbo-dev libopenjp2-7-dev libraw-dev \
    libasound2-dev libfreetype6-dev libfontconfig1-dev \
    libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev \
    imagemagick exiftool \
    dav1d ffmpeg \
    cifs-utils \
    mpv libmpv-dev \
    || fail "apt-get install failed"

ok "Dependencies installed"

# ── DRM group membership ──────────────────────────────────────────────────────
info "Adding $PRIMARY_USER to DRM/video groups..."
usermod -aG video,render "$PRIMARY_USER"
ok "$PRIMARY_USER added to video and render groups (DRM access)"

# ── DRM/KMS configuration (Pi 5) ────────────────────────────────────────────
info "Configuring DRM/KMS for Pi 5..."

BOOT_CFG="/boot/firmware/config.txt"
if [[ -f "$BOOT_CFG" ]]; then
    if ! grep -q "dtoverlay=vc4-kms-v3d" "$BOOT_CFG"; then
        echo "" >> "$BOOT_CFG"
        echo "# PiTrove DRM/KMS" >> "$BOOT_CFG"
        echo "dtoverlay=vc4-kms-v3d,cma-256" >> "$BOOT_CFG"
        echo "gpu_mem=128" >> "$BOOT_CFG"
        ok "Added vc4-kms-v3d overlay to $BOOT_CFG"
    else
        info "vc4-kms-v3d already in $BOOT_CFG"
    fi
else
    warn "$BOOT_CFG not found — manually add: dtoverlay=vc4-kms-v3d,cma-256"
fi

# ── Storage selection (NAS / Local / Network drive) ──────────────────────────
echo
info "=== Storage Configuration ==="
echo
echo "  1) NAS (SMB/CIFS network share)"
echo "  2) Local drive (USB/SD card mounted locally)"
echo "  3) Other network drive (NFS/SMB/CIFS)"
echo
read -r -p "Choose storage type [1-3]: " storage_choice < /dev/tty

USE_NAS=0
SHARE_IP=""
SHARE_MOUNT="/mnt/nas"
SHARE_PATH=""
SHARE_PROTOCOL="cifs"

case "$storage_choice" in
    1)
        USE_NAS=1
   read -r -p "NAS IP address: " SHARE_IP < /dev/tty
    if [[ -z "$SHARE_IP" ]] || ! echo "$SHARE_IP" | grep -qE '^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$'; then
        fail "Invalid IP format: '$SHARE_IP'. Must be dotted-quad (e.g. 192.168.4.111)"
    fi
       read -r -p "Share path [default: /Home/Archive]: " SHARE_PATH < /dev/tty
        SHARE_PATH="${SHARE_PATH:-/Home/Archive}"
        SHARE_PROTOCOL="cifs"
        ;;
    2)
        info "Local drive mode — no NAS mount will be created."
        info "Set your local drive mount path in config.toml after installation."
        SHARE_MOUNT="/mnt/media"
        mkdir -p "$SHARE_MOUNT"
        ok "Local mount point created: $SHARE_MOUNT"
        ;;
    3)
        echo
        echo "  a) SMB/CIFS"
        echo "  b) NFS"
        echo
        read -r -p "Choose protocol [a-b]: " proto_choice < /dev/tty
        case "$proto_choice" in
            a)
                SHARE_PROTOCOL="cifs"
                read -r -p "Network drive IP address: " SHARE_IP < /dev/tty
                read -r -p "Share path [default: /Shared]: " SHARE_PATH < /dev/tty
                SHARE_PATH="${SHARE_PATH:-/Shared}"
                ;;
            b)
                SHARE_PROTOCOL="nfs"
                read -r -p "NFS server IP [default: 192.168.4.111]: " SHARE_IP < /dev/tty
                SHARE_IP="${SHARE_IP:-192.168.4.111}"
                read -r -p "NFS export path [/mnt/nas]: " SHARE_PATH < /dev/tty
                SHARE_PATH="${SHARE_PATH:-/mnt/nas}"
                ;;
            *)
                fail "Invalid protocol choice"
                ;;
        esac
        ;;
    *)
        fail "Invalid storage choice"
        ;;
esac

# ── NAS/Network drive setup ──────────────────────────────────────────────────
if [[ "$USE_NAS" -eq 1 ]] || [[ "$storage_choice" == "3" ]]; then
    info "Configuring $SHARE_PROTOCOL mount..."

    # Check network connectivity
    if ! ping -c 1 -W 2 "$SHARE_IP" &>/dev/null; then
        warn "$SHARE_IP is not reachable — verify network connection"
    else
        ok "$SHARE_IP is reachable"
    fi

    # CIFS: prompt for credentials
    mount_creds_ok=0
    if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
        mkdir -p "$PRIMARY_HOME"
        CRED_FILE="$PRIMARY_HOME/nas.cred"
        if [[ ! -f "$CRED_FILE" ]]; then
            read -r -p "Username: " nas_user < /dev/tty
            read -rs -p "Password: " nas_pass < /dev/tty
            echo
            printf 'username=%s\npassword=%s\n' "$nas_user" "$nas_pass" > "$CRED_FILE"
            chmod 600 "$CRED_FILE"
            chown $PRIMARY_USER:$PRIMARY_USER "$CRED_FILE"
            info "Created $CRED_FILE (mode 600)"
            mount_creds_ok=1
        else
            info "NAS credentials already exist at $CRED_FILE"
            # Validate credential file contents
            if ! grep -q "^username=" "$CRED_FILE" 2>/dev/null; then
                warn "nas.cred missing username — re-entering credentials"
                read -r -p "Username: " nas_user < /dev/tty
                read -rs -p "Password: " nas_pass < /dev/tty
                echo
                printf 'username=%s\npassword=%s\n' "$nas_user" "$nas_pass" > "$CRED_FILE"
                chmod 600 "$CRED_FILE"
                chown $PRIMARY_USER:$PRIMARY_USER "$CRED_FILE"
            elif ! grep -q "^password=" "$CRED_FILE" 2>/dev/null; then
                warn "nas.cred missing password — re-entering credentials"
                read -r -p "Username: " nas_user < /dev/tty
                read -rs -p "Password: " nas_pass < /dev/tty
                echo
                printf 'username=%s\npassword=%s\n' "$nas_user" "$nas_pass" > "$CRED_FILE"
                chmod 600 "$CRED_FILE"
                chown $PRIMARY_USER:$PRIMARY_USER "$CRED_FILE"
            else
                mount_creds_ok=1
            fi
        fi
    fi

    # Build fstab entry — use uid/gid 1000 to always map to primary user
    # NOTE: _netdev ensures systemd waits for network; nofail prevents boot hang
    if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
        FSTAB_LINE="# PiTrove $SHARE_PROTOCOL mount
//$SHARE_IP/${SHARE_PATH#/} $SHARE_MOUNT cifs credentials=$PRIMARY_HOME/nas.cred,ro,uid=1000,gid=1000,vers=3.0,_netdev,nofail 0 0"
    elif [[ "$SHARE_PROTOCOL" == "nfs" ]]; then
        FSTAB_LINE="# PiTrove $SHARE_PROTOCOL mount
$SHARE_IP:$SHARE_PATH $SHARE_MOUNT $SHARE_PROTOCOL defaults,_netdev,timeo=10,retrans=3,nofail 0 0"
    fi

    # --- CLEAN OLD ENTRIES ---
    # Always remove old cifs/nfs entries for this share before adding new one
    # This prevents duplicate/old entries with deprecated options (e.g. intr, x-systemd.device-timeout)
    sed -i '/# PiTrove /d' /etc/fstab
    # Safely remove only our specific share path, not all entries from same IP
    if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
        ESCAPED_PATH=$(echo "${SHARE_PATH#/}" | sed 's/[\/]/\\&/g')
        sed -i "/^\/\/${SHARE_IP}\/${ESCAPED_PATH}/d" /etc/fstab 2>/dev/null || true
    else
        sed -i "/^${SHARE_IP}:${SHARE_PATH}/d" /etc/fstab 2>/dev/null || true
    fi
    # Add to fstab (always overwrite)
    mkdir -p "$SHARE_MOUNT"
    echo "fstab entry to add:"
    echo "  $FSTAB_LINE"
    echo "$FSTAB_LINE" >> /etc/fstab
    systemctl daemon-reload 2>/dev/null || true
    info "Added $SHARE_PROTOCOL mount to /etc/fstab"

    # Mount with retry loop
    CLEAN_SHARE="${SHARE_PATH#/}"
    info "Mounting //$SHARE_IP/$CLEAN_SHARE at $SHARE_MOUNT..."

    # Check if already mounted — skip if so
    if mountpoint -q "$SHARE_MOUNT" 2>/dev/null; then
        if ls "$SHARE_MOUNT" >/dev/null 2>&1; then
            ok "$SHARE_MOUNT already mounted, skipping mount"
            MOUNT_OK=1
        else
            warn "$SHARE_MOUNT is a mount point but empty — attempting remount"
        fi
    fi

    # Only run mount loop if not already mounted
    if [[ "$MOUNT_OK" -eq 0 ]]; then
        MOUNT_ATTEMPTS=0
        while [[ "$MOUNT_ATTEMPTS" -lt 3 ]]; do
            # Unmount any stale mount before retrying
            if mountpoint -q "$SHARE_MOUNT" 2>/dev/null; then
                umount "$SHARE_MOUNT" 2>/dev/null && info "Unmounted stale mount"
            fi

            MOUNT_ATTEMPTS=$((MOUNT_ATTEMPTS + 1))
            if mount -t "$SHARE_PROTOCOL" "$SHARE_MOUNT"; then
                ok "Mounted successfully!"
                if [[ -d "$SHARE_MOUNT" ]] && ls "$SHARE_MOUNT" >/dev/null 2>&1; then
                    info "Files found:"
                    ls "$SHARE_MOUNT" 2>/dev/null | head -10
                fi
                MOUNT_OK=1
                break
            fi

            echo
            warn "Mount failed (attempt $MOUNT_ATTEMPTS/3)"
            echo

            if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
                if [[ "$MOUNT_ATTEMPTS" -eq 3 ]]; then
                    info "After 3 failed attempts, try:"
                    info "  sudo mount -t cifs //${SHARE_IP}/${SHARE_PATH} $SHARE_MOUNT"
                    warn "Make sure credentials in $PRIMARY_HOME/nas.cred are correct"
                fi
                echo
                echo "  1) Retry mount"
                echo "  2) Change mount details"
                echo "  3) Re-enter credentials (password may be wrong)"
                echo "  4) Skip mount"
                echo
                read -r -p "Choose [1-4]: " mount_opt < /dev/tty
                case "$mount_opt" in
                    1) continue ;;
                    2)
                        read -r -p "NAS IP [$SHARE_IP]: " _tmp < /dev/tty
                        SHARE_IP="${_tmp:-$SHARE_IP}"
                        read -r -p "Share path [$SHARE_PATH]: " _tmp < /dev/tty
                        SHARE_PATH="${_tmp:-$SHARE_PATH}"
                        read -r -p "Mount point [$SHARE_MOUNT]: " _tmp < /dev/tty
                        SHARE_MOUNT="${_tmp:-/mnt/nas}"
                        if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
                            FSTAB_LINE="# PiTrove $SHARE_PROTOCOL mount
//$SHARE_IP/${SHARE_PATH#/} $SHARE_MOUNT cifs credentials=$PRIMARY_HOME/nas.cred,ro,uid=1000,gid=1000,vers=3.0,_netdev,nofail 0 0"
                        elif [[ "$SHARE_PROTOCOL" == "nfs" ]]; then
                            FSTAB_LINE="# PiTrove $SHARE_PROTOCOL mount
$SHARE_IP:$SHARE_PATH $SHARE_MOUNT $SHARE_PROTOCOL defaults,_netdev,timeo=10,retrans=3,nofail 0 0"
                        fi
                        # Clean and rewrite fstab
                        sed -i '/# PiTrove /d' /etc/fstab
                        if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
                            ESCAPED_PATH=$(echo "${SHARE_PATH#/}" | sed 's/[\/]/\\&/g')
                            sed -i "/^\/\/${SHARE_IP}\/${ESCAPED_PATH}/d" /etc/fstab 2>/dev/null || true
                        else
                            sed -i "/^${SHARE_IP}:${SHARE_PATH}/d" /etc/fstab 2>/dev/null || true
                        fi
                        echo "$FSTAB_LINE" >> /etc/fstab
                        systemctl daemon-reload 2>/dev/null || true
                        info "Updated fstab, retrying..."
                        continue
                        ;;
                    3)
                        read -r -p "Username: " nas_user < /dev/tty
                        read -rs -p "Password: " nas_pass < /dev/tty
                        echo
                        printf 'username=%s\npassword=%s\n' "$nas_user" "$nas_pass" > "$PRIMARY_HOME/nas.cred"
                        chmod 600 "$PRIMARY_HOME/nas.cred"
                        chown $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/nas.cred"
                        info "Updated credentials, retrying..."
                        continue
                        ;;
                    4)
                        warn "Skipping mount"
                        MOUNT_OK=0
                        break
                        ;;
                    *)
                        warn "Invalid option"
                        continue
                        ;;
                esac
            elif [[ "$SHARE_PROTOCOL" == "nfs" ]]; then
                echo
                echo "  1) Retry mount"
                echo "  2) Change mount details"
                echo "  3) Skip mount"
                echo
                read -r -p "Choose [1-3]: " mount_opt < /dev/tty
                case "$mount_opt" in
                    1) continue ;;
                    2)
                        read -r -p "NFS Server IP [$SHARE_IP]: " _tmp < /dev/tty
                        SHARE_IP="${_tmp:-$SHARE_IP}"
                        read -r -p "Share path [$SHARE_PATH]: " _tmp < /dev/tty
                        SHARE_PATH="${_tmp:-$SHARE_PATH}"
                        read -r -p "Mount point [$SHARE_MOUNT]: " _tmp < /dev/tty
                        SHARE_MOUNT="${_tmp:-/mnt/nas}"
                        if [[ "$SHARE_PROTOCOL" == "nfs" ]]; then
                            FSTAB_LINE="# PiTrove $SHARE_PROTOCOL mount
$SHARE_IP:$SHARE_PATH $SHARE_MOUNT $SHARE_PROTOCOL defaults,_netdev,timeo=10,retrans=3,nofail 0 0"
                        fi
                        # Clean and rewrite fstab
                        sed -i '/# PiTrove /d' /etc/fstab
                        sed -i "/^${SHARE_IP}:${SHARE_PATH}/d" /etc/fstab 2>/dev/null || true
                        echo "$FSTAB_LINE" >> /etc/fstab
                        systemctl daemon-reload 2>/dev/null || true
                        info "Updated fstab, retrying..."
                        continue
                        ;;
                    3)
                        warn "Skipping mount"
                        MOUNT_OK=0
                        break
                        ;;
                    *)
                        warn "Invalid option"
                        continue
                        ;;
                esac
            fi
        done
    fi

    if [[ "$MOUNT_OK" -eq 0 ]]; then
        warn "Could not mount $SHARE_IP:$SHARE_PATH at $SHARE_MOUNT after 3 attempts"
        if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
            warn "Manual mount command:"
            warn "  sudo mount -t cifs //${SHARE_IP}/${SHARE_PATH} $SHARE_MOUNT"
        fi
        warn "You can set it up manually later"
    else
        ok "$SHARE_PROTOCOL mount ready at $SHARE_MOUNT"
        NAS_MOUNT_SUCCESS=1
    fi
fi

# ── Git clone (if needed) ────────────────────────────────────────────────────
info "Cloning repositories..."

if [[ ! -d "$PRIMARY_HOME/piTrove/.git" ]]; then
    git clone https://github.com/UnDadFeated/piTrove.git "$PRIMARY_HOME/piTrove"
    info "Cloned piTrove repository"
else
    info "piTrove repository already exists (updating)"
    cd "$PRIMARY_HOME/piTrove"
    git pull || warn "Git pull failed"
fi
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove"

if [[ ! -d "$PRIMARY_HOME/raylib-src/.git" ]]; then
    git clone https://github.com/raysan5/raylib.git "$PRIMARY_HOME/raylib-src"
    info "Cloned raylib repository"
else
    info "raylib repository already exists (skipping)"
fi
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/raylib-src"

# ── Install fonts ────────────────────────────────────────────────────────────
info "Installing CRT overlay fonts..."

   if [[ -d "$PRIMARY_HOME/piTrove/src/fonts" ]]; then
        mkdir -p /usr/share/fonts/truetype/dejavu
        if [[ ! -f /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf ]]; then
            cp "$PRIMARY_HOME/piTrove/src/fonts/DejaVuSansMono-Bold.ttf" /usr/share/fonts/truetype/dejavu/
            ok "Installed DejaVu Sans Mono Bold"
    else
        info "Font already installed (skipping)"
    fi
    fc-cache -fv 2>/dev/null || true
fi

ok "Fonts installed"

# ── Build raylib (DRM platform) ─────────────────────────────────────────────
info "Building raylib (DRM platform)..."

cd "$PRIMARY_HOME/raylib-src"
rm -rf build
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DPLATFORM=DRM \
    -DGRAPHICS=GRAPHICS_API_OPENGL_ES2 \
    -DCMAKE_C_FLAGS="-O3 -march=native -mtune=native"
cmake --build build -j3 || fail "raylib build failed"

cp build/raylib/libraylib.a /usr/local/lib/libraylib.a
cp src/raylib.h  /usr/local/include/raylib.h
cp src/rlgl.h    /usr/local/include/rlgl.h
cp src/raymath.h /usr/local/include/raymath.h
ok "Installed raylib to /usr/local/lib/ + /usr/local/include/"

# ── Build piTrove ────────────────────────────────────────────────────────────
info "Building piTrove..."

cd "$PRIMARY_HOME/piTrove/src"
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j3 || fail "piTrove build failed"

cp ./build/piTrove $PRIMARY_HOME/piTrove/piTrove
chown $PRIMARY_USER:$PRIMARY_USER $PRIMARY_HOME/piTrove/piTrove
info "Installed piTrove to $PRIMARY_HOME/piTrove/piTrove"

cd "$PRIMARY_HOME/piTrove"

# ── Splash image ─────────────────────────────────────────────────────────────
# Splash stays in src/ — config.toml points to it directly
info "Splash image is in src/splash.png"

# ── Directory structure ──────────────────────────────────────────────────────
info "Creating directory structure..."

sudo -u "$PRIMARY_USER" mkdir -p "$PRIMARY_HOME/.cache/piTrove"
sudo -u "$PRIMARY_USER" mkdir -p "$PRIMARY_HOME/piTrove/src/config"
sudo -u "$PRIMARY_USER" mkdir -p "$PRIMARY_HOME/piTrove/logs"
sudo -u "$PRIMARY_USER" mkdir -p "$PRIMARY_HOME/piTrove/src/fonts"
mkdir -p "$SHARE_MOUNT"

# ── Scan window selection ────────────────────────────────────────────────────
read -r -p "Temporal window (=/- days from today, year agnostic, 0=disable) [default: 15]: " scan_input < /dev/tty

if [[ -z "$scan_input" ]]; then
    SCAN_WINDOW_DAYS=15
elif [[ "$scan_input" =~ ^[0-9]+$ ]]; then
    SCAN_WINDOW_DAYS="$scan_input"
else
    warn "Invalid input — using default: 15 days"
    SCAN_WINDOW_DAYS=15
fi

ok "Scan window set to: $SCAN_WINDOW_DAYS days"

# ── Write config ─────────────────────────────────────────────────────────────
info "Writing config..."

# Warn if existing config would be overwritten, backup first
if [[ -f "$PRIMARY_HOME/piTrove/src/config/config.toml" ]]; then
    warn "Existing config.toml found — backing up and replacing with defaults"
    cp "$PRIMARY_HOME/piTrove/src/config/config.toml" "$PRIMARY_HOME/piTrove/src/config/config.toml.bak.$(date +%Y%m%d%H%M%S)"
fi

cat > "$PRIMARY_HOME/piTrove/src/config/config.toml" <<EOF
# ==========================================
# piTrove Configuration File (v7.0.0)
# ==========================================

[paths]
media_dir = $SHARE_MOUNT
cache_dir = $PRIMARY_HOME/.cache/piTrove
log_dir = $PRIMARY_HOME/piTrove/logs

[display]
rotation = 0
splash_file = src/splash.png
splash_overlay_y = 0.5

[slideshow]
transition_delay = 120.0
transition_duration = 1.5
slideshow_fps = 30
transition_effect = "crossfade"
ken_burns = 0
ken_burns_speed = 0.1
matting = 1
matting_size = 48
cooldown_days = 330
brightness_auto = 0
brightness_auto_min = 50
brightness_auto_max = 100
bias_lighting = 1
 bias_anim_speed = 0.5
 bias_anim_style = "edge_glow"
 bias_color_mode = "auto"
 ken_burns_zoom = 0.15
 bias_strength = 110
 
 [scan]
recursive = 1
depth = 10
max_concurrent = 4
window_days = $SCAN_WINDOW_DAYS
ignore_folders = ["@eaDir", "@Recycle", "Thumbs.db"]

[sqlite]
mmap_size = 67108864

[log]
level = info

[overlay]
timer_enabled = 1
timer_x = 0.94
timer_y = 0.03
timer_font_size = 12
timer_color = yellow
filename_enabled = 1
filename_x = 0.04
filename_y = 0.966
count_enabled = 0
videos_per_photos = 3
sleep_time = ""
wake_time = ""

[date_overlay]
enabled = 0
text = "%Y-%m-%d"
x = 0.1
y = 0.08
font_size = 20
color = cyan

[touch]
enabled = 0

[collage]
enabled = 0
cols = 2
rows = 2

[video]
volume = 0
probe_timeout = 5

[dashboard]
weather_enabled = 0
weather_lat = -999.0
weather_lon = -999.0

[remote]
http_enabled = 0
http_port = 8080
EOF

chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove/src/config"
ok "Config written to $PRIMARY_HOME/piTrove/src/config/config.toml"

# ── Ensure all directories have correct ownership ──
mkdir -p "$PRIMARY_HOME/.cache/piTrove"
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/.cache/piTrove"
mkdir -p "$PRIMARY_HOME/piTrove/logs"
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove/logs"
mkdir -p "$PRIMARY_HOME/piTrove/src/fonts"
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove/src/fonts"

# Warn if media_dir points to unmounted path
if [[ "$USE_NAS" -eq 1 || "$storage_choice" == "3" ]]; then
    if [[ "$NAS_MOUNT_SUCCESS" -ne 1 ]]; then
        warn "media_dir ($SHARE_MOUNT) may be inaccessible — NAS not mounted"
        warn "  After manual mount, update media_dir in config.toml or run: sudo mount -a"
    fi
fi

# ── systemd service ──────────────────────────────────────────────────────────
info "Installing systemd service..."

cat > /etc/systemd/system/piTrove.service <<EOF
[Unit]
Description=PiTrove Digital Picture Frame
After=multi-user.target network-online.target
Wants=network-online.target
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=simple
User=$PRIMARY_USER
Group=$PRIMARY_USER
WorkingDirectory=$PRIMARY_HOME/piTrove
ExecStart=$PRIMARY_HOME/piTrove/piTrove --config $PRIMARY_HOME/piTrove/src/config/config.toml
Restart=always
RestartSec=15
StandardOutput=journal
StandardError=journal
Environment=HOME=$PRIMARY_HOME

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable piTrove.service
info "Service installed: piTrove.service (enabled)"

 # ── Done ─────────────────────────────────────────────────────────────────────
echo
echo "============================================"
 echo "  PiTrove v7.0.0 installation complete!"
echo "============================================"
echo

# Conditional next steps based on mount outcome
echo "  Next steps:"
if [[ "$storage_choice" -eq 2 ]]; then
    echo "    NAS: ✓ Local storage"
    echo "    1. Start UI: piTrove --config (runs TUI wizard)"
    echo "    2. Auto-start is enabled (reboot to test)"
elif [[ "$USE_NAS" -eq 1 || "$storage_choice" == "3" ]]; then
    if [[ "$NAS_MOUNT_SUCCESS" -eq 1 ]]; then
        echo "    NAS: ✓ Already mounted at /mnt/nas"
        echo "    1. Start UI: piTrove --config (runs TUI wizard)"
        echo "    2. Auto-start is enabled (reboot to test)"
    else
        echo "    NAS: ✗ Not mounted — configure manually"
        echo "    1. Add to /etc/fstab"
        echo "       Example: //192.168.4.111/Home/Archive /mnt/nas cifs credentials=$PRIMARY_HOME/nas.cred,ro,uid=1000,gid=1000,vers=3.0,_netdev,nofail 0 0"
        echo "    2. Run: sudo mount -a"
        echo "    3. Start UI: piTrove --config (runs TUI wizard)"
        echo "    4. Auto-start is enabled (reboot to test)"
    fi
fi
echo
echo "  Directories:"
echo "  Config:       $PRIMARY_HOME/piTrove/src/config/config.toml"
echo "  Source:       $PRIMARY_HOME/piTrove/src/"
echo "  Cache:        $PRIMARY_HOME/.cache/piTrove/"
echo "  Logs:         $PRIMARY_HOME/piTrove/logs/"
echo
