#!/usr/bin/env bash
# install.sh — piTrove v18.0.7 Premium Graphical Installer
# Target: Debian Trixie (13) 64-bit on Raspberry Pi 4/5

set -eo pipefail

# Root check - must be first operational check
if [[ "$(id -u)" -ne 0 ]]; then
    fail "This script must be run as root (sudo)"
fi

# ── Graphical & Color Palette (256-color and modern ANSI) ──────────────────────
RED='\033[38;5;203m'
GREEN='\033[38;5;120m'
YELLOW='\033[38;5;221m'
BLUE='\033[38;5;75m'
MAGENTA='\033[38;5;171m'
CYAN='\033[38;5;51m'
WHITE='\033[38;5;231m'
GRAY='\033[38;5;244m'
DARK_GRAY='\033[38;5;237m'
NC='\033[0m'
BOLD='\033[1m'
UNDERLINE='\033[4m'

# Box drawing characters
BOX_TL="╔"
BOX_TR="╗"
BOX_BL="╚"
BOX_BR="╝"
BOX_HL="═"
BOX_VL="║"
BOX_ML="╠"
BOX_MR="╣"

# ── Signal Traps for Terminal Restoration ──────────────────────────────────────
cleanup_terminal() {
    tput cnorm 2>/dev/null || echo -ne "\033[?25h"
}
trap cleanup_terminal EXIT INT TERM

# ── Visual Helper Functions ───────────────────────────────────────────────────

banner() {
    clear 2>/dev/null || true
    echo -e " ${BOLD}${WHITE}piTrove${NC}  ${CYAN}v17.7.10${NC} ${GRAY}──────────────────────────${NC} ${BOLD}${GREEN}Installer${NC}"
    echo -e " ${MAGENTA}The Ultra-Premium Picture Frame${NC}"
    echo
}

info()  { echo -e "   ${CYAN}[ ℹ ]${NC}  $*"; }
warn()  { echo -e "   ${YELLOW}[ ⚠ ]${NC}  $*"; }
ok()    { echo -e "   ${GREEN}[ ✓ ]${NC}  $*"; }

fail()  {
    echo
    echo -e " ${RED}✘ ERROR: $*${NC}"
    echo
    exit 1
}

# Track non-fatal warnings (QR, media, config wizard)
G_EXIT_CODE=0

# ── Safe Read Function for Piped Execution ────────────────────────────────────
safe_read() {
    if [[ -t 0 ]]; then
        read "$@"
    elif { true < /dev/tty; } 2>/dev/null; then
        read "$@" < /dev/tty
    else
        read "$@" < /dev/null
    fi
}

yesno() {
    echo -e -n "\n   ${BOLD}${YELLOW}▸ $* [y/N]:${NC} "
    safe_read -r resp 
    [[ "$resp" == [yY]* ]]
}

# ── Dynamic Spinner (Braille Unicode Animation) ─────────────────────────────────
show_spinner() {
    local pid=$1
    local label="$2"
    local log_file="${3:-}"
    local delay=0.05
    local spin_chars=("⠋" "⠙" "⠹" "⠸" "⠼" "⠴" "⠦" "⠧" "⠇" "⠏")
    local i=0
    local last_status=""
    local max_width=64
    local start_time
    start_time=$(date +%s)

    tput civis 2>/dev/null || echo -ne "\033[?25l"

    while kill -0 "$pid" 2>/dev/null; do
        local char="${spin_chars[i]}"

        # Update status line from log file every tick
        if [[ -n "$log_file" && -f "$log_file" ]]; then
            local new_status
            new_status=$(tr "\r" "\n" < "$log_file" 2>/dev/null | grep -v "^[[:space:]]*$" | sed "s/\x1b\[[0-9;]*m//g" | tail -n 1 | tr -d "\n\r" | head -c "$max_width" || true)
            if [[ -n "$new_status" ]]; then
                last_status="$new_status"
            fi
        fi

        # Calculate elapsed time
        local elapsed=$(( $(date +%s) - start_time ))
        local elapsed_str
        if [[ $elapsed -ge 60 ]]; then
            elapsed_str="$(( elapsed / 60 ))m $(( elapsed % 60 ))s"
        else
            elapsed_str="${elapsed}s"
        fi

        # Grey subline: show elapsed time AND latest verbose log activity
        local subline
        if [[ -n "$last_status" ]]; then
            subline="${elapsed_str} elapsed | ${last_status}"
        else
            subline="${elapsed_str} elapsed..."
        fi

        # Print spinner line, always two lines so cursor stays fixed
        printf "\r\033[K   ${CYAN}[%s]${NC}  %s...                        \n" "$char" "$label"
        printf "\r\033[K      ${GRAY}▸ %s${NC}" "$subline"
        printf "\033[1A"

        i=$(( (i + 1) % 10 ))
        sleep $delay
    done

    true # ensure exit code 0
    tput cnorm 2>/dev/null || echo -ne "\033[?25h"
    printf "\r\033[K\n\r\033[K\033[1A"
}

run_with_spinner() {
    local label="$1"
    shift
    local log_file=$(mktemp /tmp/pitrove_cmd.XXXXXX.log)
    
    if [[ "${IS_CRON:-0}" -eq 1 ]] || [[ ! -t 1 ]]; then
        # Running under cron or non-interactive context, run directly without spinner
        "$@" > "$log_file" 2>&1
        local status=$?
        if [[ "$status" -ne 0 ]]; then
            echo -e "   ${RED}[ ✘ ]  ${label} failed!${NC}"
            echo -e "   ${YELLOW}─────── LAST 15 LINES OF LOG: ───────${NC}"
            tail -n 15 "$log_file" | sed 's/^/   /'
            echo -e "   ${YELLOW}─────────────────────────────────────${NC}"
            fail "${label} failed with exit code ${status}. Check ${log_file} for details."
        else
            ok "${label} completed successfully"
            rm -f "$log_file"
        fi
    else
        # Interactive mode, show spinner
        stdbuf -oL -eL "$@" > "$log_file" 2>&1 &
        local pid=$!
        
        show_spinner "$pid" "$label" "$log_file"
        
        wait "$pid"
        local status=$?
        if [[ "$status" -ne 0 ]]; then
            echo -e "   ${RED}[ ✘ ]  ${label} failed!${NC}"
            echo -e "   ${YELLOW}─────── LAST 15 LINES OF LOG: ───────${NC}"
            tail -n 15 "$log_file" | sed 's/^/   /'
            echo -e "   ${YELLOW}─────────────────────────────────────${NC}"
            fail "${label} failed with exit code ${status}. Check ${log_file} for details."
        else
            ok "${label} completed successfully"
            rm -f "$log_file"
        fi
    fi
}


# ── Render Initial Screen ─────────────────────────────────────────────────────
banner

# 1. Detect primary user (fallback if UID 1000 is modified or in non-interactive/cron contexts)
if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
    PRIMARY_USER="$SUDO_USER"
else
    SCRIPT_OWNER=$(stat -c '%U' "$0" 2>/dev/null || true)
    if [[ -n "$SCRIPT_OWNER" && "$SCRIPT_OWNER" != "root" ]]; then
        PRIMARY_USER="$SCRIPT_OWNER"
    else
        PRIMARY_USER=$(getent passwd 1000 | cut -d: -f1 || true)
    fi
fi

if [[ -z "$PRIMARY_USER" ]]; then
    PRIMARY_USER=$(logname 2>/dev/null || echo $USER)
fi

if [[ "$PRIMARY_USER" == "root" ]]; then
    fail "Installer detected 'root'. Please run from a standard user account with sudo."
fi
PRIMARY_HOME="/home/$PRIMARY_USER"
info "Primary user: ${BOLD}${WHITE}$PRIMARY_USER${NC} (${CYAN}$PRIMARY_HOME${NC})"

# OS check
OS_OK=0
if [[ -f /etc/os-release ]]; then
    . /etc/os-release
    if [[ "$ID" == "debian" && "$VERSION_ID" == "13" ]]; then
        OS_OK=1
    fi
fi
if [[ "$OS_OK" -ne 1 ]] && command -v lsb_release &>/dev/null; then
    if [[ "$(lsb_release -si)" == "Debian" && "$(lsb_release -rs)" == "13" ]]; then
        OS_OK=1
    fi
fi
if [[ "$OS_OK" -ne 1 ]]; then
    fail "This installer requires Debian Trixie (13) 64-bit"
fi
if [[ "$(uname -m)" != "aarch64" ]]; then
    fail "This installer requires ARM64 (aarch64), found $(uname -m)"
fi
ok "Debian Trixie 64-bit validated successfully"

# 2. Bootstrap packages (git, lsb_release, curl, sudo needed below)
run_with_spinner "Updating system package repositories" apt-get update -qq
run_with_spinner "Installing bootstrap tools" apt-get install -y -qq git curl lsb-release sudo
# Install QR code generator for terminal dashboard URL
run_with_spinner "Installing QR encoder" apt-get install -y -qq qrencode 2>/dev/null || true

# Bootstrap Docker
if ! command -v docker &>/dev/null; then
    
info "Installing network filesystem utilities & base dependencies..."
apt-get update -qq
apt-get install -y -qq ca-certificates cifs-utils nfs-common 2>/dev/null || true

run_with_spinner "Installing Docker Engine" sh -c "curl -fsSL https://get.docker.com | sh"
fi
if ! docker compose version &>/dev/null; then
    run_with_spinner "Installing Docker Compose Plugin" apt-get install -y -qq docker-compose-plugin
fi
# NetworkManager is required by the piTrove keepalive thread (nmcli device connect)
# and by the Wi-Fi power-saving override section below
run_with_spinner "Installing NetworkManager" apt-get install -y -qq network-manager
systemctl enable --now NetworkManager &>/dev/null || true
ok "NetworkManager installed and enabled"

# Parse a value from config.toml
# Usage: get_config_val "section" "key" "default"
get_config_val() {
    local sec="$1"
    local key="$2"
    local default="$3"
    local file="$PRIMARY_HOME/piTrove/config/config.toml"
    if [[ ! -f "$file" ]]; then
        echo "$default"
        return
    fi
    local val
    val=$(awk -v sec="[$sec]" -v key="$key" '
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", $0);
        tsec = sec; gsub(/^[[:space:]]+|[[:space:]]+$/, "", tsec);
        in_sec = ($0 == tsec)
        in_sec && $0 ~ "^[[:space:]]*"key"[[:space:]]*=" {
            sub(/^[^=]*=[[:space:]]*/, "");
            sub(/[[:space:]]*$/, "");
            gsub(/^"|"$/, "");
            print;
            exit;
        }
    ' "$file")
    echo "${val:-$default}"
}

# ── Handle Organize Archive Command Line Option ────────────────────────────────
if [[ "$1" == "--organize" ]]; then
    if [[ -z "$2" ]]; then
        fail "Missing target folder. Usage: install.sh --organize <folder_path>"
    fi
    
    TARGET_DIR=$(realpath "$2" 2>/dev/null || echo "$2")
    if [[ ! -d "$TARGET_DIR" ]]; then
        fail "Folder does not exist: $2"
    fi
    
    echo -e " ${BOLD}${WHITE}Media Archive Reorganization${NC}"
    echo -e "   Target directory: ${GREEN}$TARGET_DIR${NC}"
    echo
    echo -e "   Please select an organization strategy:"
    echo -e "   ${BOLD}${GREEN}1)${NC} ${BOLD}Chronological Folders${NC}"
    echo -e "      Move files into Photos/YYYY-MM/ and Videos/YYYY-MM/ directories."
    echo -e "   ${BOLD}${GREEN}2)${NC} ${BOLD}In-Place Prefix${NC}"
    echo -e "      Add the YYYY-MM-DD_ prefix to files inside their existing folder structures."
    echo -e "   ${BOLD}${GREEN}3)${NC} ${BOLD}Disable Seasonal Scanning${NC}"
    echo -e "      Keep all folders as is, but disable the seasonal window in config.toml"
    echo -e "      so all files will play without filtering."
    echo -e "   ${BOLD}${GREEN}4)${NC} ${BOLD}Abort${NC}"
    echo
    
    safe_read -p "▸ Enter choice [1-4]: " STRATEGY
    
    if [[ "$STRATEGY" == "3" ]]; then
        info "Disabling seasonal scanning window in config.toml..."
        CFG_PATH="$PRIMARY_HOME/piTrove/config/config.toml"
        if [[ -f "$CFG_PATH" ]]; then
            sed -i 's/^window_days\s*=.*/window_days = 0/' "$CFG_PATH"
            ok "Set window_days = 0 in $CFG_PATH"
            info "Restarting background slideshow daemon to reload config..."
            systemctl restart piTrove.service &>/dev/null || true
            ok "Slideshow daemon restarted successfully."
        else
            fail "Configuration file not found at $CFG_PATH"
        fi
        exit 0
    elif [[ "$STRATEGY" == "4" ]] || [[ -z "$STRATEGY" ]]; then
        info "Aborted by user."
        exit 0
    elif [[ "$STRATEGY" != "1" && "$STRATEGY" != "2" ]]; then
        fail "Invalid strategy selected."
    fi
    
    MOUNT_POINT=$(df --output=target "$TARGET_DIR" | tail -n 1)
    
    echo -e "${RED}[!] WARNING: The target archive is located on $MOUNT_POINT.${NC}"
    echo -e "    This operation will:"
    echo -e "    1. Temporarily remount '$MOUNT_POINT' from Read-Only (ro) to Read-Write (rw)."
    if [[ "$STRATEGY" == "1" ]]; then
        echo -e "    2. Scan, rename, and group all photos and videos under:"
        echo -e "       - $TARGET_DIR/Photos/YYYY-MM/YYYY-MM-DD_filename.ext"
        echo -e "       - $TARGET_DIR/Videos/YYYY-MM/YYYY-MM-DD_filename.ext"
    else
        echo -e "    2. Scan and rename all files in-place with the 'YYYY-MM-DD_' prefix"
        echo -e "       within their current directories."
    fi
    echo -e "    3. Retain original file timestamps and resolve naming conflicts."
    echo -e "    4. Restore '$MOUNT_POINT' to Read-Only (ro) mount mode on completion."
    echo ""
    
    safe_read -p "▸ Are you sure you want to proceed? (y/N): " CONF1
    if [[ ! "$CONF1" =~ ^[Yy]$ ]]; then
        info "Aborted by user."
        exit 0
    fi
    
    safe_read -p "▸ CONFIRM ONCE MORE: Are you absolutely sure? This will rewrite files on the storage. (y/N): " CONF2
    if [[ ! "$CONF2" =~ ^[Yy]$ ]]; then
        info "Aborted by user."
        exit 0
    fi
    
    IS_RO=0
    if findmnt -n -o OPTIONS "$MOUNT_POINT" | grep -q "ro"; then
        IS_RO=1
        info "Remounting $MOUNT_POINT as Read-Write (rw)..."
        # Only remount if it's a cifs or nfs mount
        if mountpoint -q "$MOUNT_POINT" 2>/dev/null && (findmnt -n -o FSTYPE "$MOUNT_POINT" | grep -qiE "cifs|nfs"); then
            mount -o remount,rw "$MOUNT_POINT"
            if [[ $? -ne 0 ]]; then
                fail "Failed to remount $MOUNT_POINT as read-write."
            fi
        else
            warn "Mount point $MOUNT_POINT is not a cifs/nfs mount, skipping remount."
        fi
    fi
    
    # Map host target directory to container path
    HOST_MEDIA_DIR="${MEDIA_DIR:-/mnt/nas}"
    CONTAINER_DIR="$TARGET_DIR"
    if [[ "$CONTAINER_DIR" == "$HOST_MEDIA_DIR"* ]]; then
        CONTAINER_DIR="/app/media${CONTAINER_DIR#$HOST_MEDIA_DIR}"
    fi

    # Use -it only if a TTY is available (non-interactive contexts like cron would fail)
    DOCKER_IT="-i"
    if [[ -t 0 ]] || { true < /dev/tty; } 2>/dev/null; then
        DOCKER_IT="-it"
    fi

    if [[ "$STRATEGY" == "1" ]]; then
        info "Organizing media archive (Chronological Folders) inside docker at $CONTAINER_DIR..."
        docker exec $DOCKER_IT piTrove /app/piTrove --organize "$CONTAINER_DIR"
    else
        info "Organizing media archive (In-Place Prefix) inside docker at $CONTAINER_DIR..."
        docker exec $DOCKER_IT piTrove /app/piTrove --organize "$CONTAINER_DIR" --in-place
    fi
    
    if [[ "$IS_RO" -eq 1 ]]; then
        info "Restoring $MOUNT_POINT mount mode to Read-Only (ro)..."
        mount -o remount,ro "$MOUNT_POINT"
        if [[ $? -ne 0 ]]; then
            fail "Failed to restore $MOUNT_POINT to read-only mount. Please restore manually!"
        fi
        ok "$MOUNT_POINT successfully restored to Read-Only mode."
    fi
    exit 0
fi

# ── Handle Update Command Line Option ──────────────────────────────────────────
if [[ "$1" == "--update" ]]; then
    # Change directory to the repository to run git commands
    cd "$PRIMARY_HOME/piTrove" || fail "Failed to enter repository directory: $PRIMARY_HOME/piTrove"

    IS_CRON=0
    if [[ "$2" == "--cron" ]]; then
        IS_CRON=1
    fi

    CONFIG_FILE="$PRIMARY_HOME/piTrove/config/config.toml"
    
    AUTO_UPD_ENABLED=$(get_config_val "updates" "auto_update" "0")
    CONFIG_BRANCH=$(get_config_val "updates" "auto_update_branch" "main")
    
    if [[ "$IS_CRON" -eq 1 ]]; then
        if [[ "$AUTO_UPD_ENABLED" != "1" && "$AUTO_UPD_ENABLED" != "true" ]]; then
            # Auto-update not enabled in config, skip cron execution silently
            exit 0
        fi
    fi

    info "Initiating piTrove Update Checker (Branch: $CONFIG_BRANCH)..."
    
    # Verify we are inside a git repository
    if ! sudo -u "$PRIMARY_USER" git rev-parse --is-inside-work-tree &>/dev/null; then
        fail "Not in a valid Git repository. Please run --update from within the piTrove repository folder."
    fi
    
    # Perform git fetch safely as the primary user
    run_with_spinner "Fetching latest repository states from origin" sudo -u "$PRIMARY_USER" git fetch --all --prune
    
    # Checkout configured branch if not already on it
    CURRENT_BRANCH=$(sudo -u "$PRIMARY_USER" git rev-parse --abbrev-ref HEAD)
    if [[ "$CURRENT_BRANCH" != "$CONFIG_BRANCH" ]]; then
        run_with_spinner "Switching branch to $CONFIG_BRANCH" sudo -u "$PRIMARY_USER" git checkout "$CONFIG_BRANCH"
    fi
    
    LOCAL_COMMIT=$(sudo -u "$PRIMARY_USER" git rev-parse HEAD)
    REMOTE_COMMIT=$(sudo -u "$PRIMARY_USER" git rev-parse @{u} 2>/dev/null || echo "")
    
    if [[ -z "$REMOTE_COMMIT" ]]; then
        fail "No remote upstream branch tracked. Cannot verify remote updates."
    fi
    
    if [[ "$LOCAL_COMMIT" == "$REMOTE_COMMIT" ]]; then
        ok "piTrove is already up-to-date at the latest version!"
        exit 0
    fi
    
    info "Updates discovered! Upgrading repository from commit ${CYAN}${LOCAL_COMMIT:0:7}${NC} to ${GREEN}${REMOTE_COMMIT:0:7}${NC}..."
    
    # Reset latest changes to match remote branch cleanly
    run_with_spinner "Pulling latest changes from remote branch" sudo -u "$PRIMARY_USER" git reset --hard "origin/$CONFIG_BRANCH"
    
    # Rebuild docker compose container image
    run_with_spinner "Rebuilding container image" docker compose build
    
    # Restart systemd service if it exists and is active
    if systemctl list-unit-files | grep -q piTrove.service; then
        run_with_spinner "Restarting background daemon (piTrove.service)" systemctl restart piTrove.service
    fi
    
    echo
    echo -e " ${BOLD}${GREEN}✔  piTrove Updated and Deployed Successfully${NC}"
    echo
    exit 0
fi

# ── Kill Conflicting Display Servers ───────────────────────────────────────────
for svc in labwc-tty1 seatd; do
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
        run_with_spinner "Stopping conflicting display service ($svc)" systemctl stop "$svc"
        run_with_spinner "Disabling conflicting display service ($svc)" systemctl disable "$svc"
    fi
done

# ── Stop Existing piTrove Processes & Services ────────────────────────────────
if systemctl is-active --quiet piTrove.service 2>/dev/null; then
    run_with_spinner "Stopping active piTrove.service" systemctl stop piTrove.service
fi
if systemctl is-enabled --quiet piTrove.service 2>/dev/null; then
    run_with_spinner "Disabling piTrove.service" systemctl disable piTrove.service
fi

info "Clearing existing process conflicts..."
pkill -9 piTrove || true

info "Removing deprecated desktop autostarts..."
rm -f "$PRIMARY_HOME/.config/autostart/piTrove.desktop" 2>/dev/null || true
rm -f "/etc/xdg/autostart/piTrove.desktop" 2>/dev/null || true

# 5b. Safeguard SSH & Configure Keep-Alives (prevent idle SSH/SFTP/rclone mount freezes)
if ! systemctl is-active --quiet ssh 2>/dev/null; then
    warn "SSH service is inactive! Restoring to prevent lockout..."
    apt-get install -y -qq openssh-server 2>/dev/null || true
    systemctl enable ssh 2>/dev/null || true
    systemctl start ssh 2>/dev/null || true
    if systemctl is-active --quiet ssh 2>/dev/null; then
        ok "SSH safeguard restored successfully"
    else
        warn "Could not start SSH service! Verify SSH manually."
    fi
fi

if [[ -f "/etc/ssh/sshd_config" ]]; then
    info "Configuring SSH server-side keep-alives to prevent client/rclone idle dropouts..."
    # Remove any existing ClientAliveInterval/ClientAliveCountMax lines to avoid duplicates
    sed -i '/^ClientAliveInterval/d' /etc/ssh/sshd_config
    sed -i '/^ClientAliveCountMax/d' /etc/ssh/sshd_config
    # Add the keep-alive settings
    echo "ClientAliveInterval 60" >> /etc/ssh/sshd_config
    echo "ClientAliveCountMax 3" >> /etc/ssh/sshd_config
    systemctl restart ssh 2>/dev/null || true
    ok "SSH keep-alives configured persistently (60s interval)"
fi

# 6. Disk space check
AVAIL=$(df --output=avail / | tail -n 1 | tr -d ' ')
if [[ "$AVAIL" -lt 5242880 ]]; then
    warn "Low disk space: Only $((AVAIL / 1024))MB free on / (recommended: 5GB for cache/compiling)"
else
    ok "Disk space check passed ($((AVAIL / 1024 / 1024))GB available)"
fi

# ── Install host filesystem dependencies (NAS mounts) ──────────────────────────
run_with_spinner "Installing host filesystem dependencies (cifs-utils, nfs-common)" apt-get install -y -qq cifs-utils nfs-common 2>/dev/null || true

# ── DRM and Docker group configuration ─────────────────────────────────────────
run_with_spinner "Adding $PRIMARY_USER to video, render, and docker groups for hardware & docker permission" usermod -aG video,render,docker "$PRIMARY_USER"

# ── DRM/KMS firmware configuration (Pi 4/5) ───────────────────────────────────
BOOT_CFG="/boot/firmware/config.txt"
if [[ -f "$BOOT_CFG" ]]; then
    if ! grep -q "cma-" "$BOOT_CFG"; then
        if ! grep -q "dtoverlay=vc4-kms-v3d" "$BOOT_CFG"; then
            echo "" >> "$BOOT_CFG"
            echo "# piTrove DRM/KMS Display Configuration" >> "$BOOT_CFG"
            echo "dtoverlay=vc4-kms-v3d,cma-512" >> "$BOOT_CFG"
            echo "gpu_mem=128" >> "$BOOT_CFG"
            ok "Configured vc4-kms-v3d overlay & cma-512 in $BOOT_CFG"
        else
            sed -i s/dtoverlay=vc4-kms-v3d.*/dtoverlay=vc4-kms-v3d,cma-512/ "$BOOT_CFG"
            ok "Updated vc4-kms-v3d overlay with cma-512 in $BOOT_CFG"
        fi
    else
        ok "CMA memory overlay already configured in $BOOT_CFG"
    fi
else
    warn "$BOOT_CFG not found. Manually verify vc4-kms-v3d overlay is loaded."
fi

# ── Wi-Fi Power Saving Override Fail-safe ────────────────────────────────────
info "Configuring Wi-Fi power-saving overrides..."
if [[ -d "/etc/NetworkManager/conf.d" ]]; then
    cat > /etc/NetworkManager/conf.d/default-wifi-powersave-on.conf <<EOF
[connection]
wifi.powersave = 2
EOF
    # systemctl restart NetworkManager &>/dev/null &
    ok "Disabled NetworkManager Wi-Fi Power Saving persistently"
else
    ok "NetworkManager not active, skipping power-saving overrides"
fi

# Driver-level persistent Wi-Fi power-saving disable via udev rule
cat > /etc/udev/rules.d/81-wifi-powersave.rules <<EOF
ACTION=="add", SUBSYSTEM=="net", KERNEL=="wlan*", RUN+="/usr/sbin/iw dev %k set power_save off"
EOF
# Apply power save off immediately to any active wlan interfaces
for dev in /sys/class/net/wlan*; do
    if [[ -d "$dev" ]]; then
        iface=$(basename "$dev")
        /usr/sbin/iw dev "$iface" set power_save off &>/dev/null || true
    fi
done
ok "Configured persistent driver-level Wi-Fi power-saving overrides"



# ── Branch Selection Dialog ────────────────────────────────────────────────────
echo
echo -e " ${BOLD}${WHITE}Select Installation Branch${NC}"
echo
echo -e " ${BOLD}${GREEN}1)${NC} ${WHITE}main${NC} (Recommended) — Stable production release"
echo -e "     - Production-ready, tested and verified"
echo -e " ${BOLD}${GREEN}2)${NC} ${WHITE}develop${NC} — Active development, features under test"
echo -e "     - Cutting-edge updates, may contain experimental features"
echo -e " ${BOLD}${GREEN}3)${NC} ${WHITE}Use currently checked-out branch"
echo -e "     - Use whatever branch is already checked out locally"
echo
info "${BOLD}Default: main${NC} (press Enter to accept)"
echo -n -e "   ${BOLD}${YELLOW}▸ Enter your choice [1-3]:${NC} "
safe_read -r branch_choice
branch_choice="${branch_choice:-1}"

INSTALL_BRANCH="main"
BRANCH_LABEL="main"
case "$branch_choice" in
    1)
        INSTALL_BRANCH="main"
        BRANCH_LABEL="main"
        ;;
    2)
        INSTALL_BRANCH="develop"
        BRANCH_LABEL="develop"
        ;;
    3)
        if [[ -d "$PRIMARY_HOME/piTrove/.git" ]]; then
            INSTALL_BRANCH=$(cd "$PRIMARY_HOME/piTrove" 2>/dev/null && sudo -u "$PRIMARY_USER" git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
            if [[ -z "$INSTALL_BRANCH" ]]; then
                INSTALL_BRANCH="main"
                BRANCH_LABEL="main"
            else
                BRANCH_LABEL="$INSTALL_BRANCH"
            fi
        else
            INSTALL_BRANCH="main"
            BRANCH_LABEL="main"
        fi
        ;;
    *)
        warn "Invalid choice. Defaulting to main branch."
        INSTALL_BRANCH="main"
        BRANCH_LABEL="main"
        ;;
esac

if [[ "$BRANCH_LABEL" == "main" ]]; then
    info "${GREEN}✓${NC} Installation branch: ${BOLD}${GREEN}main${NC} (stable production)"
else
    info "${YELLOW}⚠${NC} Installation branch: ${BOLD}${YELLOW}${BRANCH_LABEL}${NC} (development/testing)"
fi

# ── Clone / Update Git Repository ──────────────────────────────────────────────
info "Setting up piTrove repository clone..."
if [[ ! -d "$PRIMARY_HOME/piTrove/.git" ]]; then
    if [[ -d "$PRIMARY_HOME/piTrove" ]]; then
        warn "Directory $PRIMARY_HOME/piTrove exists but is not a valid Git repository. Cleaning it up for a fresh clone..."
        rm -rf "$PRIMARY_HOME/piTrove"
    fi
    run_with_spinner "Cloning piTrove repository (branch: $INSTALL_BRANCH)" sudo -u "$PRIMARY_USER" git clone --branch "$INSTALL_BRANCH" --single-branch https://github.com/UnDadFeated/piTrove.git "$PRIMARY_HOME/piTrove"
else
    info "Repository exists. Updating source..."
    cd "$PRIMARY_HOME/piTrove"
    CURRENT_BRANCH=$(sudo -u "$PRIMARY_USER" git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
    if [[ "$CURRENT_BRANCH" != "$INSTALL_BRANCH" ]]; then
        info "Switching from branch '$CURRENT_BRANCH' to '$INSTALL_BRANCH'..."
        sudo -u "$PRIMARY_USER" git fetch origin || true
        sudo -u "$PRIMARY_USER" git checkout "$INSTALL_BRANCH" || true
    fi
    sudo -u "$PRIMARY_USER" git pull origin "$INSTALL_BRANCH" || warn "Repository update failed. Using active local copy."
fi
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove"
info "Repository ready at: ${CYAN}$PRIMARY_HOME/piTrove${NC} (branch: ${BOLD}${BRANCH_LABEL}${NC})"

# ── Wi-Fi Keepalive Daemon & Organizer Cleanups (v13.0.0 Migration) ───────────
info "Migrating keepalive and organizer to native C++23 implementations..."

# Remove legacy Wi-Fi keepalive cron jobs
if crontab -u "$PRIMARY_USER" -l 2>/dev/null | grep -q "wifi_keepalive.sh"; then
    crontab -u "$PRIMARY_USER" -l 2>/dev/null | grep -v "wifi_keepalive.sh" | crontab -u "$PRIMARY_USER" - || true
    info "Removed legacy Wi-Fi keepalive cron job from $PRIMARY_USER"
fi
if crontab -l 2>/dev/null | grep -q "wifi_keepalive.sh"; then
    crontab -l 2>/dev/null | grep -v "wifi_keepalive.sh" | crontab - || true
    info "Removed legacy Wi-Fi keepalive cron job from root"
fi

# Remove legacy scripts if they exist
rm -f "$PRIMARY_HOME/piTrove/scripts/wifi_keepalive.sh"
rm -f "$PRIMARY_HOME/piTrove/scripts/organize.py"
ok "Legacy scripts and cron jobs cleaned up successfully"

# ── Config Merger Script Setup ───────────────────────────────────────────────
info "Configuring settings file merger script..."
cat > "$PRIMARY_HOME/piTrove/scripts/merge_config.py" <<'EOF'
#!/usr/bin/env python3
import sys
import os

def parse_toml(filepath):
    if not os.path.exists(filepath):
        return {}
    config = {}
    current_section = None
    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or line.startswith(';'):
                continue
            if line.startswith('[') and line.endswith(']'):
                current_section = line[1:-1].strip()
                config[current_section] = {}
                continue
            if '=' in line:
                key, val = line.split('=', 1)
                key = key.strip()
                val = val.strip()
                if current_section is not None:
                    config[current_section][key] = val
    return config

def merge_configs(template_path, user_path, output_path):
    if not os.path.exists(template_path):
        print(f"Error: Template config not found at {template_path}")
        return False
        
    template_config = parse_toml(template_path)
    user_config = parse_toml(user_path)
    
    merged_lines = []
    current_section = None
    
    with open(template_path, 'r') as f:
        for line in f:
            stripped = line.strip()
            if stripped.startswith('[') and stripped.endswith(']'):
                current_section = stripped[1:-1].strip()
                merged_lines.append(line)
                continue
            if '=' in stripped and not stripped.startswith('#') and not stripped.startswith(';'):
                key, _ = stripped.split('=', 1)
                key = key.strip()
                
                if current_section in user_config and key in user_config[current_section]:
                    user_val = user_config[current_section][key]
                    indent = line[:len(line) - len(line.lstrip())]
                    merged_lines.append(f"{indent}{key} = {user_val}\n")
                else:
                    merged_lines.append(line)
                continue
            
            merged_lines.append(line)
            
    tmp_path = output_path + ".tmp"
    try:
        with open(tmp_path, 'w') as f:
            f.writelines(merged_lines)
        os.replace(tmp_path, output_path)
    except Exception as e:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        print(f"Error: Failed to write merged config atomically: {e}")
        return False
    print(f"[✓] Config merge completed: {user_path} updated with latest options.")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(1)
    success = merge_configs(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else sys.argv[2])
    if not success:
        sys.exit(1)
EOF
chmod +x "$PRIMARY_HOME/piTrove/scripts/merge_config.py"
chown $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove/scripts/merge_config.py"
ok "Configured settings file merger script"


# ── Storage Selection & Verification Loop ──────────────────────────────────────
while true; do
    USE_NAS=0
    SHARE_IP=""
    SHARE_MOUNT="/mnt/nas"
    SHARE_PATH=""
    SHARE_PROTOCOL="cifs"

    echo
    echo -e " ${BOLD}${WHITE}Select Storage Mode${NC}"
    echo
    echo -e " ${BOLD}${GREEN}1)${NC} ${WHITE}NAS (SMB/CIFS Network Share)${NC}"
    echo -e "     - Mounts your remote server's archive to /mnt/nas"
    echo
    echo -e " ${BOLD}${GREEN}2)${NC} ${WHITE}Local Drive (USB / MicroSD)${NC}"
    echo -e "     - Keeps all assets stored locally on the Pi"
    echo
    echo -e " ${BOLD}${GREEN}3)${NC} ${WHITE}Other Network Drive (NFS/Custom)${NC}"
    echo -e "     - Custom setup options for NFS or other mounts"
    echo
    if [[ -n "$STORAGE_CHOICE" ]]; then
        storage_choice="$STORAGE_CHOICE"
        info "Using storage choice from environment: $storage_choice"
        STORAGE_CHOICE="" # Clear to prompt on retry
    else
        echo -n -e "   ${BOLD}${YELLOW}▸ Enter your choice [1-3]:${NC} "
        safe_read -r storage_choice
    fi

    case "$storage_choice" in
        1)
            USE_NAS=1
            if [[ -n "$NAS_IP" ]]; then
                SHARE_IP="$NAS_IP"
                info "Using NAS IP from environment: $SHARE_IP"
            else
                echo -n -e "\n   ${BOLD}${CYAN}▸ NAS IP Address:${NC} "
                safe_read -r SHARE_IP
            fi
            if [[ -z "$SHARE_IP" ]] || ! echo "$SHARE_IP" | grep -qE '^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$'; then
                warn "Invalid IP format: '$SHARE_IP'. Must be dotted-quad (e.g. 192.168.4.110)"
                continue
            fi
            if [[ -n "$NAS_SHARE" ]]; then
                SHARE_PATH="$NAS_SHARE"
                info "Using NAS share path from environment: $SHARE_PATH"
            else
                echo -n -e "   ${BOLD}${CYAN}▸ Share path [default: /Home/Archive]:${NC} "
                safe_read -r SHARE_PATH
            fi
            SHARE_PATH="${SHARE_PATH:-/Home/Archive}"
            SHARE_PROTOCOL="cifs"
            ;;
        2)
            if [[ -n "$LOCAL_MEDIA_PATH" ]]; then
                SHARE_MOUNT="$LOCAL_MEDIA_PATH"
                info "Using local media path from environment: $SHARE_MOUNT"
            else
                echo -n -e "   ${BOLD}${CYAN}▸ Local media directory [default: /mnt/media]:${NC} "
                safe_read -r SHARE_MOUNT
                SHARE_MOUNT="${SHARE_MOUNT:-/mnt/media}"
            fi
            mkdir -p "$SHARE_MOUNT"
            ok "Local media directory initialized at $SHARE_MOUNT"
            ;;
        3)
            echo
            echo -e " ${BOLD}${WHITE}Select Protocol${NC}"
            echo
            echo -e " ${BOLD}${GREEN}a)${NC} ${WHITE}SMB/CIFS Network Share${NC}"
            echo -e " ${BOLD}${GREEN}b)${NC} ${WHITE}NFS Network Share${NC}"
            echo
            echo -n -e "      ${BOLD}${YELLOW}▸ Select protocol [a-b]:${NC} "
            safe_read -r proto_choice 
            case "$proto_choice" in
                a)
                    SHARE_PROTOCOL="cifs"
                    echo -n -e "\n      ${BOLD}${CYAN}▸ SMB Server IP Address:${NC} "
                    safe_read -r SHARE_IP 
                    echo -n -e "      ${BOLD}${CYAN}▸ SMB Share path [default: /Shared]:${NC} "
                    safe_read -r SHARE_PATH 
                    SHARE_PATH="${SHARE_PATH:-/Shared}"
                    ;;
                b)
                    SHARE_PROTOCOL="nfs"
                    echo -n -e "\n      ${BOLD}${CYAN}▸ NFS Server IP Address [default: 192.168.4.111]:${NC} "
                    safe_read -r SHARE_IP 
                    SHARE_IP="${SHARE_IP:-192.168.4.111}"
                    echo -n -e "      ${BOLD}${CYAN}▸ NFS Export path [default: /mnt/nas]:${NC} "
                    safe_read -r SHARE_PATH 
                    SHARE_PATH="${SHARE_PATH:-/mnt/nas}"
                    ;;
                *)
                    warn "Invalid protocol choice"
                    continue
                    ;;
            esac
            ;;
        *)
            warn "Invalid storage selection"
            continue
            ;;
    esac

    # ── NAS/Network Mount Setup ────────────────────────────────────────────────────
    NAS_MOUNT_SUCCESS=0
    if [[ "$USE_NAS" -eq 1 ]] || [[ "$storage_choice" == "3" ]]; then
        info "Configuring network service mount point..."

        # Check network connection
        if ! ping -c 1 -W 2 "$SHARE_IP" &>/dev/null; then
            warn "Host $SHARE_IP is not reachable! Continuing anyway, but double-check your network."
        else
            ok "Host $SHARE_IP is online"
        fi

        # CIFS Credentials setup
        if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
            mkdir -p "$PRIMARY_HOME"
            CRED_FILE="$PRIMARY_HOME/nas.cred"
            if [[ ! -f "$CRED_FILE" ]]; then
                if [[ -n "$NAS_USER" && -n "$NAS_PASS" ]]; then
                    nas_user="$NAS_USER"
                    nas_pass="$NAS_PASS"
                    info "Using NAS credentials from environment"
                else
                    echo -n -e "\n   ${BOLD}${YELLOW}▸ Username:${NC} "
                    safe_read -r nas_user 
                    echo -n -e "   ${BOLD}${YELLOW}▸ Password:${NC} "
                    safe_read -rs nas_pass 
                    echo
                fi
                printf 'username=%s\npassword=%s\n' "$nas_user" "$nas_pass" > "$CRED_FILE"
                chmod 600 "$CRED_FILE"
                chown $PRIMARY_USER:$PRIMARY_USER "$CRED_FILE"
                ok "Created credentials configuration file: $CRED_FILE"
            else
                info "NAS credential file already exists at $CRED_FILE"
                if [[ -n "$NAS_USER" && -n "$NAS_PASS" ]]; then
                    nas_user="$NAS_USER"
                    nas_pass="$NAS_PASS"
                    info "Updating NAS credentials from environment"
                    printf 'username=%s\npassword=%s\n' "$nas_user" "$nas_pass" > "$CRED_FILE"
                    chmod 600 "$CRED_FILE"
                    chown $PRIMARY_USER:$PRIMARY_USER "$CRED_FILE"
                elif ! grep -q "^username=" "$CRED_FILE" 2>/dev/null || ! grep -q "^password=" "$CRED_FILE" 2>/dev/null; then
                    warn "Credential file is incomplete. Re-entering credentials..."
                    echo -n -e "   ${BOLD}${YELLOW}▸ Username:${NC} "
                    safe_read -r nas_user 
                    echo -n -e "   ${BOLD}${YELLOW}▸ Password:${NC} "
                    safe_read -rs nas_pass 
                    echo
                    printf 'username=%s\npassword=%s\n' "$nas_user" "$nas_pass" > "$CRED_FILE"
                    chmod 600 "$CRED_FILE"
                    chown $PRIMARY_USER:$PRIMARY_USER "$CRED_FILE"
                fi
            fi
        fi

        # Format fstab configuration row
        generate_fstab_line() {
            if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
                FSTAB_LINE="# piTrove Network Share
//$SHARE_IP/${SHARE_PATH#/} $SHARE_MOUNT cifs credentials=$PRIMARY_HOME/nas.cred,ro,uid=1000,gid=1000,vers=3.0,_netdev,nofail,x-systemd.mount-timeout=30,hard,echo_interval=60,actimeo=30 0 0"
            elif [[ "$SHARE_PROTOCOL" == "nfs" ]]; then
                FSTAB_LINE="# piTrove Network Share
$SHARE_IP:$SHARE_PATH $SHARE_MOUNT $SHARE_PROTOCOL defaults,_netdev,timeo=10,retrans=3,nofail,x-systemd.mount-timeout=30,hard 0 0"
            fi
        }
        generate_fstab_line

        # Clean old entries
        sed -i '/# piTrove /d' /etc/fstab
        sed -i '/# PiTrove /d' /etc/fstab
        # Clean any existing mount entry to the same mount point to avoid duplicates
        sed -i "\|[[:space:]]$SHARE_MOUNT[[:space:]]|d" /etc/fstab 2>/dev/null || true
        if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
            ESCAPED_PATH=$(echo "${SHARE_PATH#/}" | sed 's/[\/]/\\&/g')
            sed -i "/^\/\/${SHARE_IP}\/${ESCAPED_PATH}/d" /etc/fstab 2>/dev/null || true
        else
            sed -i "/^${SHARE_IP}:${SHARE_PATH}/d" /etc/fstab 2>/dev/null || true
        fi

        mkdir -p "$SHARE_MOUNT"
        echo "$FSTAB_LINE" >> /etc/fstab
        systemctl daemon-reload 2>/dev/null || true
        ok "Added fstab mounting entry for $SHARE_MOUNT"

        # Mount retry handler
        CLEAN_SHARE="${SHARE_PATH#/}"
        MOUNT_OK=0

        if mountpoint -q "$SHARE_MOUNT" 2>/dev/null; then
            if ls "$SHARE_MOUNT" >/dev/null 2>&1; then
                ok "$SHARE_MOUNT is already active and mounted"
                MOUNT_OK=1
                NAS_MOUNT_SUCCESS=1
            fi
        fi

        if [[ "$MOUNT_OK" -eq 0 ]]; then
            MOUNT_ATTEMPTS=0
            while [[ "$MOUNT_ATTEMPTS" -lt 3 ]]; do
                if mountpoint -q "$SHARE_MOUNT" 2>/dev/null; then
                    umount "$SHARE_MOUNT" 2>/dev/null || true
                fi
                
                MOUNT_ATTEMPTS=$((MOUNT_ATTEMPTS + 1))
                info "Mounting Share (Attempt $MOUNT_ATTEMPTS/3)..."
                
                if mount "$SHARE_MOUNT" 2>/dev/null; then
                    ok "Mount completed successfully!"
                    MOUNT_OK=1
                    NAS_MOUNT_SUCCESS=1
                    break
                fi

                warn "Mount failed (attempt $MOUNT_ATTEMPTS/3)"
                echo
                if [[ "$MOUNT_ATTEMPTS" -eq 3 ]]; then
                    warn "Cannot mount network share automatically."
                    echo -e " ${BOLD}${WHITE}Mount Failed — Choose Option${NC}"
                    echo
                    echo -e " ${BOLD}${GREEN}1)${NC} ${WHITE}Retry connection${NC}"
                    echo -e " ${BOLD}${GREEN}2)${NC} ${WHITE}Re-enter path configuration${NC}"
                    echo -e " ${BOLD}${GREEN}3)${NC} ${WHITE}Re-enter username & password${NC}"
                    echo -e " ${BOLD}${GREEN}4)${NC} ${WHITE}Skip and mount manually later${NC}"
                    echo
                    echo -n -e "      ${BOLD}${YELLOW}▸ Choose option [1-4]:${NC} "
                    safe_read -r mount_opt 
                    case "$mount_opt" in
                        1) MOUNT_ATTEMPTS=0; continue ;;
                        2)
                            echo -n -e "      ${BOLD}${CYAN}▸ NAS IP [$SHARE_IP]:${NC} "
                            safe_read -r _tmp ; SHARE_IP="${_tmp:-$SHARE_IP}"
                            echo -n -e "      ${BOLD}${CYAN}▸ Share Path [$SHARE_PATH]:${NC} "
                            safe_read -r _tmp ; SHARE_PATH="${_tmp:-$SHARE_PATH}"
                            generate_fstab_line
                            sed -i '/# piTrove /d' /etc/fstab
                            echo "$FSTAB_LINE" >> /etc/fstab
                            systemctl daemon-reload 2>/dev/null || true
                            MOUNT_ATTEMPTS=0
                            continue
                            ;;
                        3)
                            if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
                                echo -n -e "      ${BOLD}${YELLOW}▸ Username:${NC} "
                                safe_read -r nas_user 
                                echo -n -e "      ${BOLD}${YELLOW}▸ Password:${NC} "
                                safe_read -rs nas_pass 
                                echo
                                printf 'username=%s\npassword=%s\n' "$nas_user" "$nas_pass" > "$PRIMARY_HOME/nas.cred"
                                chmod 600 "$PRIMARY_HOME/nas.cred"
                                chown $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/nas.cred"
                            fi
                            MOUNT_ATTEMPTS=0
                            continue
                            ;;
                        4)
                            warn "Skipping active mount check. Remount later using 'sudo mount -a'"
                            MOUNT_OK=1
                            break
                            ;;
                        *)
                            warn "Invalid option. Skipping..."
                            MOUNT_OK=1
                            break
                            ;;
                    esac
                fi
            done
        fi
    fi

    # ── Folder Verification and Preview ────────────────────────────────────────────
    if [[ -d "$SHARE_MOUNT" ]]; then
        echo
        info "Verifying content access in target folder: ${BOLD}$SHARE_MOUNT${NC}"
        
        file_list=$(ls -A "$SHARE_MOUNT" 2>/dev/null | head -n 10)
        
        if [[ -n "$file_list" ]]; then
            info "First few items detected (up to 10):"
            echo "$file_list" | sed 's/^/     • /'
            echo
            safe_read -p "▸ Is this the correct directory to scan recursively? (Y/n): " scan_ok
            if [[ ! "$scan_ok" =~ ^[Nn]$ ]]; then
                break
            else
                info "Re-entering configuration..."
                if [[ "$USE_NAS" -eq 1 || "$storage_choice" == "3" ]]; then
                    if mountpoint -q "$SHARE_MOUNT" 2>/dev/null; then
                        umount "$SHARE_MOUNT" 2>/dev/null || true
                    fi
                fi
            fi
        else
            warn "Target directory is EMPTY or inaccessible."
            if yesno "Use this empty directory anyway and configure manually later?"; then
                break
            else
                info "Re-entering configuration..."
                if [[ "$USE_NAS" -eq 1 || "$storage_choice" == "3" ]]; then
                    if mountpoint -q "$SHARE_MOUNT" 2>/dev/null; then
                        umount "$SHARE_MOUNT" 2>/dev/null || true
                    fi
                fi
            fi
        fi
    else
        warn "Target directory does not exist or is not a folder: $SHARE_MOUNT"
        if yesno "Create and use this directory anyway?"; then
            mkdir -p "$SHARE_MOUNT"
            break
        fi
    fi
done

# ── Media Archive Prefix Detection ───────────────────────────────────────────
echo
echo -e " ${BOLD}${WHITE}Media Archive Prefix Detection${NC}"
echo -e "   Target archive directory: ${GREEN}$SHARE_MOUNT${NC}"

HAS_PREFIX=0
if [[ -d "$SHARE_MOUNT" ]]; then
    if find "$SHARE_MOUNT" -maxdepth 3 -regextype posix-extended -regex '.*/[0-9]{4}[-_][0-9]{2}([-_][0-9]{2})?(_.*)?' -print -quit 2>/dev/null | grep -q .; then
        HAS_PREFIX=1
    fi
fi

if [[ "$HAS_PREFIX" -eq 1 ]]; then
    ok "Date-based folder or filename prefixes detected in media library."
else
    warn "Folder or filename YYYY-MM-DD_ prefix was not found, using file attributes for seasonal window."
fi

# ── Font Setup ────────────────────────────────────────────────────────────────
info "Configuring application typography..."
if [[ -d "$PRIMARY_HOME/piTrove/src/fonts" ]]; then
    mkdir -p /usr/share/fonts/truetype/dejavu
    if [[ ! -f /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf ]]; then
        cp "$PRIMARY_HOME/piTrove/src/fonts/DejaVuSansMono-Bold.ttf" /usr/share/fonts/truetype/dejavu/
        ok "Loaded DejaVu Sans Mono Bold font"
    fi
    fc-cache -fv &>/dev/null || true
fi
ok "Fonts system configuration complete"

# ── Create Directories ────────────────────────────────────────────────────────
# Create bind-mount target directories before Docker build/run to ensure
# they exist with correct ownership (otherwise Docker creates them as root)
info "Creating internal directory tree..."
mkdir -p "$PRIMARY_HOME/piTrove/cache" "$PRIMARY_HOME/piTrove/config"
mkdir -p "$PRIMARY_HOME/piTrove/src/config"
mkdir -p "$PRIMARY_HOME/piTrove/logs"
mkdir -p "$PRIMARY_HOME/piTrove/subtitles"
mkdir -p "$PRIMARY_HOME/piTrove/src/fonts"
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove/cache" "$PRIMARY_HOME/piTrove/config" "$PRIMARY_HOME/piTrove/logs" "$PRIMARY_HOME/piTrove/subtitles"

# ── Build piTrove Docker Container ─────────────────────────────────────────────
info "Building piTrove Docker Container (compiling binary inside container)..."
cd "$PRIMARY_HOME/piTrove"

# Probe active DRM card
PROBED_CARD=$(find /sys/class/drm/ -name "card*-*" -exec grep -q "^connected$" {}/status \; -print -quit | sed -E 's|.*/(card[0-9]+)-.*|\1|')
if [ -z "$PROBED_CARD" ]; then
    PROBED_CARD=$(ls /dev/dri/card* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)
fi
if [ -z "$PROBED_CARD" ]; then
    PROBED_CARD="card1"
fi
PROBED_INDEX=${PROBED_CARD#card}

# Write local .env file first for docker-compose to use during build/run
echo "SDL_VIDEO_KMSDRM_DEVICE=/dev/dri/$PROBED_CARD" > .env
echo "SDL_KMSDRM_DEVICE_INDEX=$PROBED_INDEX" >> .env
echo "MEDIA_DIR=$SHARE_MOUNT" >> .env
chown $PRIMARY_USER:$PRIMARY_USER .env

run_with_spinner "Building piTrove container image" docker compose build
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/.docker" 2>/dev/null || true

# ── Scan Window Setup ──────────────────────────────────────────────────────────
echo
if [[ -n "$SCAN_WINDOW_DAYS" ]]; then
    scan_input="$SCAN_WINDOW_DAYS"
    info "Using scan window days from environment: $scan_input"
else
    echo -n -e "   ${BOLD}${YELLOW}▸ Temporal window (current month +/- days, 0=disable) [default: 5]:${NC} "
    safe_read -r scan_input
fi

if [[ -z "$scan_input" ]]; then
    SCAN_WINDOW_DAYS=5
elif [[ "$scan_input" =~ ^[0-9]+$ ]]; then
    SCAN_WINDOW_DAYS="$scan_input"
else
    warn "Invalid format. Defaulting to 5 days."
    SCAN_WINDOW_DAYS=5
fi
ok "Scan window initialized to $SCAN_WINDOW_DAYS days"

# ── Google Photos Interactive Setup ──────────────────────────────────────────────
echo
GOOGLE_PHOTOS_ENABLED=0
GP_CLIENT_ID=""
GP_CLIENT_SECRET=""
GP_ALBUM_ID=""
GP_SYNC_INTERVAL=60

if yesno "Configure Google Photos cloud integration?"; then
    GOOGLE_PHOTOS_ENABLED=1
    
    # Validate Client ID
    while true; do
        echo -n -e "   ${BOLD}${YELLOW}▸ Google Photos Client ID:${NC} "
        safe_read -r GP_CLIENT_ID
        if [[ -z "$GP_CLIENT_ID" ]]; then
            warn "Client ID cannot be empty. Please enter your Google Cloud OAuth Client ID."
        elif [[ ! "$GP_CLIENT_ID" =~ \.apps\.googleusercontent\.com$ ]]; then
            warn "Client ID does not end with '.apps.googleusercontent.com'. Double check your credential entry."
            if yesno "Use this Client ID anyway?"; then
                break
            fi
        else
            break
        fi
    done

    # Validate Client Secret
    while true; do
        echo -n -e "   ${BOLD}${YELLOW}▸ Google Photos Client Secret:${NC} "
        safe_read -r GP_CLIENT_SECRET
        if [[ -z "$GP_CLIENT_SECRET" ]]; then
            warn "Client Secret cannot be empty. Please enter your Google Cloud OAuth Client Secret."
        else
            break
        fi
    done

    echo -n -e "   ${BOLD}${YELLOW}▸ Google Photos Album ID (optional, press Enter for all):${NC} "
    safe_read -r GP_ALBUM_ID

    # Validate Sync Interval
    while true; do
        echo -n -e "   ${BOLD}${YELLOW}▸ Sync Interval in minutes [default: 60]:${NC} "
        safe_read -r GP_SYNC_INT_INPUT
        GP_SYNC_INTERVAL="${GP_SYNC_INT_INPUT:-60}"
        if [[ "$GP_SYNC_INTERVAL" =~ ^[0-9]+$ ]] && [[ "$GP_SYNC_INTERVAL" -ge 1 ]] && [[ "$GP_SYNC_INTERVAL" -le 1440 ]]; then
            break
        else
            warn "Invalid interval. Please specify an integer between 1 and 1440 minutes."
        fi
    done

    ok "Google Photos configured (will require browser authorization at http://<IP>:9000/google_photos_setup)"
fi

# ── Configuration TOML ─────────────────────────────────────────────────────────
info "Writing configuration options..."

# Probing network gateway for default route
PROBED_GATEWAY=$(ip route | grep '^default' | awk '{print $3}' | head -n1 || true)
PROBED_INTERFACE=$(ip route | grep '^default' | awk '{print $5}' | head -n1 || true)
if [[ -z "$PROBED_GATEWAY" ]]; then
    PROBED_GATEWAY="192.168.4.1" # Fallback
fi
if [[ -z "$PROBED_INTERFACE" ]]; then
    PROBED_INTERFACE="wlan0" # Fallback
fi

CONFIG_FILE="$PRIMARY_HOME/piTrove/config/config.toml"

if [[ -f "$CONFIG_FILE" ]]; then
    warn "Existing config.toml detected. Preserving custom user settings..."
    BACKUP_FILE="$CONFIG_FILE.bak.$(date +%Y%m%d%H%M%S)"
    cp "$CONFIG_FILE" "$BACKUP_FILE"
    info "Merging updates into existing config.toml..."
    
    # Run configuration updates. If any fail, restore backup and exit.
    (
        set -e
        python3 "$PRIMARY_HOME/piTrove/scripts/merge_config.py" "$PRIMARY_HOME/piTrove/src/config.toml" "$CONFIG_FILE" 
        sed -i "s/^window_days = .*/window_days = $SCAN_WINDOW_DAYS/" "$CONFIG_FILE"

        # Apply interactive changes if Google Photos was configured
        if [[ "$GOOGLE_PHOTOS_ENABLED" -eq 1 ]]; then
            sed -i "/\[google_photos\]/,/^\[/ s/^enabled = .*/enabled = $GOOGLE_PHOTOS_ENABLED/" "$CONFIG_FILE"
            sed -i "/\[google_photos\]/,/^\[/ s/^client_id = .*/client_id = \"$GP_CLIENT_ID\"/" "$CONFIG_FILE"
            sed -i "/\[google_photos\]/,/^\[/ s/^client_secret = .*/client_secret = \"$GP_CLIENT_SECRET\"/" "$CONFIG_FILE"
            sed -i "/\[google_photos\]/,/^\[/ s/^album_id = .*/album_id = \"$GP_ALBUM_ID\"/" "$CONFIG_FILE"
            sed -i "/\[google_photos\]/,/^\[/ s/^sync_interval_mins = .*/sync_interval_mins = $GP_SYNC_INTERVAL/" "$CONFIG_FILE"
            # Ensure HTTP remote control is enabled to receive callback redirects
            sed -i "/\[remote\]/,/^\[/ s/^http_enabled = .*/http_enabled = 1/" "$CONFIG_FILE"
        fi
    )
    if [[ $? -ne 0 ]]; then
        error "Config updates failed! Restoring backup config from $BACKUP_FILE"
        cp "$BACKUP_FILE" "$CONFIG_FILE"
        exit 1
    fi
else
    cat > "$CONFIG_FILE" <<EOF
# ==========================================
# piTrove Configuration File (auto-generated)
# ==========================================

[paths]
media_dir = "/app/media"
cache_dir = "/app/cache"
log_dir = "/app/logs"

[display]
rotation = 0
splash_file = "src/splash.png"
splash_overlay_y = 0.5
bg_style = "pattern"
pattern_brightness = 45
pattern_style = "random_animated"
pattern_blend_count = 2
pattern_fps = 10

[slideshow]
transition_delay = 120.0
transition_duration = 1.5
slideshow_fps = 30
transition_effect = "crossfade"
ken_burns = 0
ken_burns_speed = 0.1
matting = 1
matting_size = 96
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
edge_glow_shadow = 1

[scan]
recursive = 1
depth = 10
max_concurrent = 4
window_days = $SCAN_WINDOW_DAYS
ignore_folders = ["@eaDir", "@Recycle", "Thumbs.db"]

[sqlite]
mmap_size = 67108864

[log]
level = "info"

[overlay]
timer_enabled = 1
timer_x = 0.94
timer_y = 0.03
timer_font_size = 12
timer_color = "yellow"
filename_enabled = 1
filename_x = 0.04
filename_y = 0.966
count_enabled = 0
videos_per_photos = 3
play_just_photos = 0
play_just_videos = 0
show_people_faces = 1
keep_animals = 1
sleep_time = ""
wake_time = ""
fan_speed = 30

[date_overlay]
enabled = 0
text = "%Y-%m-%d"
x = 0.1
y = 0.08
font_size = 20
color = "cyan"

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
weather_lat = 34.05
weather_lon = -118.24

[remote]
http_enabled = 1
http_port = 9000

[mqtt]
enabled = 0
broker = "192.168.4.111"
port = 1883
user = ""
pass = ""
topic_prefix = "piTrove"
motionsensor_topic = "home/motionsensor"
motionsensor_cooldown = 60

[google_photos]
enabled = $GOOGLE_PHOTOS_ENABLED
client_id = "$GP_CLIENT_ID"
client_secret = "$GP_CLIENT_SECRET"
refresh_token = ""
album_id = "$GP_ALBUM_ID"
sync_interval_mins = $GP_SYNC_INTERVAL
cache_dir = "/app/cache/google_photos"

[updates]
auto_update = 0
auto_update_branch = "main"

[keepalive]
enabled = 1
interval_secs = 120
gateway_ip = "$PROBED_GATEWAY"
wifi_interface = "$PROBED_INTERFACE"
EOF
fi

chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove/config"
ok "Default production config.toml generated"

# Ensure watchdog directory exists
mkdir -p "$PRIMARY_HOME/piTrove/src/watchdog"

# Final ownership pass to catch any root-created files during build
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove"

# ── systemd Service Deployment ─────────────────────────────────────────────────
info "Installing daemon background service..."
info "Systemd: Using probed DRM GPU device /dev/dri/$PROBED_CARD (index $PROBED_INDEX)"

# Sourcing correct KMSDRM parameters for stable SDL video playback
cat > /etc/systemd/system/piTrove.service <<EOF
[Unit]
Description=PiTrove Docker Digital Picture Frame
After=multi-user.target network-online.target docker.service
Wants=network-online.target docker.service
StartLimitIntervalSec=0
StartLimitBurst=0

[Service]
Type=simple
User=$PRIMARY_USER
Group=$PRIMARY_USER
WorkingDirectory=$PRIMARY_HOME/piTrove
ExecStartPre=-/bin/bash -c "ls /mnt/nas >/dev/null 2>&1 || true"
ExecStartPre=-/usr/bin/docker compose down
ExecStart=/usr/bin/docker compose up
ExecStop=/usr/bin/docker compose down
Restart=always
RestartSec=15
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
# systemctl enable piTrove.service   # deferred until user starts it &>/dev/null
# systemctl start piTrove.service   # deferred until user starts it &>/dev/null || true
ok "piTrove.service registered (starts on first launch)"

# ── Configure Auto-Update Cron Job ─────────────────────────────────────────────
info "Configuring auto-update cron job..."
CRON_JOB="0 3 * * * $PRIMARY_HOME/piTrove/install.sh --update --cron >/dev/null 2>&1"
CURRENT_CRON=$(crontab -l 2>/dev/null | grep -v "piTrove/install.sh --update" || true)
(echo "$CURRENT_CRON"; echo "$CRON_JOB") | crontab -
ok "Auto-update cron job configured persistently (runs daily at 3:00 AM)"

# ── pitrove CLI Command Wrapper ─────────────────────────────────────────────────
info "Installing 'pitrove' CLI management tool..."
cat > /usr/local/bin/pitrove <<'EOF'
#!/usr/bin/env bash

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

show_help() {
    echo -e "${CYAN}piTrove CLI Management Utility${NC}"
    echo -e "Usage: ${BLUE}pitrove [command]${NC}"
    echo ""
    echo "Commands:"
    echo -e "  ${GREEN}config${NC}   - Launch the 11-category interactive terminal settings wizard"
    echo -e "  ${GREEN}restart${NC}  - Safely restart the piTrove background slideshow daemon"
    echo -e "  ${GREEN}logs${NC}     - View/tail live application rendering & load logs"
    echo -e "  ${GREEN}status${NC}   - View current status of the background container service"
    echo -e "  ${GREEN}help${NC}     - Show this utility help guide"
}

case "$1" in
    config)
        if [[ ! -t 0 ]] && { true < /dev/tty; } 2>/dev/null; then
            exec 0< /dev/tty
        fi
        docker exec -it piTrove /app/piTrove --config-wizard /app/config/config.toml || G_EXIT_CODE=2
        stty sane 2>/dev/null || true
        tput cnorm 2>/dev/null || true
        echo
        ;;
    restart)
        echo -e "${YELLOW}[▸] Restarting piTrove service...${NC}"
        sudo systemctl restart piTrove.service
        echo -e "${GREEN}[✓] Restart command dispatched.${NC}"
        ;;
    logs)
        docker logs -f piTrove
        ;;
    status)
        docker ps -f name=piTrove
        ;;
    *)
        show_help
        ;;
esac
EOF
chmod +x /usr/local/bin/pitrove
ok "'pitrove' CLI command wrapper successfully installed at /usr/local/bin/pitrove"
# ── Network Watchdog Service (External to Docker) ──────────────────────────────
info "Installing piTrove network watchdog service..."
mkdir -p /etc/pitrove
cat << EOF > /etc/pitrove/wdog.conf
GATEWAY="$PROBED_GATEWAY"
INTERFACE="$PROBED_INTERFACE"
CIFS_MOUNT="$SHARE_MOUNT"
DOCKER_CONTAINER="piTrove"
CIFS_REMOUNT_COOLDOWN=60
EOF

# Copy watchdog script
cp "$PRIMARY_HOME/piTrove/src/watchdog/pitrove-watchdog.sh" /usr/local/bin/pitrove-watchdog.sh
chmod +x /usr/local/bin/pitrove-watchdog.sh

# Install systemd service unit
cp "$PRIMARY_HOME/piTrove/src/watchdog/pitrove-watchdog.service" /etc/systemd/system/pitrove-watchdog.service

systemctl daemon-reload
systemctl enable pitrove-watchdog.service &>/dev/null
systemctl start pitrove-watchdog.service &>/dev/null || true
ok "pitrove-watchdog.service successfully registered, enabled & started"


# ── Cleanup Bootstrap File ─────────────────────────────────────────────────────
BOOTSTRAP="$PRIMARY_HOME/install.sh"
if [[ -f "$BOOTSTRAP" ]]; then
    rm -f "$BOOTSTRAP"
    info "Removed temporary bootstrap copy"
fi


# ── Successful Completion Dashboard ───────────────────────────────────────────
clear 2>/dev/null || true
banner

print_success_card() {
    local url="http://$(hostname -I | awk '{print $1}' 2>/dev/null || ip route get 8.8.8.8 2>/dev/null | awk '{print $7}' || echo "127.0.0.1"):9000/"
    if [[ "$url" == "http://:9000/" ]]; then url="http://127.0.0.1:9000/"; fi

    echo
    echo -e " ${BOLD}${GREEN}✔  Installation Completed Successfully!${NC}"
    echo
    echo -e " ${BOLD}${WHITE}Path Locations:${NC}"
    echo -e "   • Container Base:  ${CYAN}/home/pi/piTrove/${NC}"
    echo -e "   • Configuration:   ${CYAN}config/config.toml${NC}"
    echo -e "   • SQLite Cache:    ${CYAN}cache/cache.db${NC}"
    echo -e "   • Service Logs:    ${CYAN}logs/piTrove_*.log${NC}"
    echo
    echo -e " ${BOLD}${WHITE}Web Remote Dashboard & MQTT HUD:${NC}"
    echo -e "   • URL: ${BOLD}${CYAN}$url${NC}"
    echo -e "     Click to view MQTT telemetry, control the screen physically,"
    echo -e "     and trigger motion simulation sweeps remotely."
    echo
    echo -e " ${BOLD}${WHITE}Manage & Control (CLI Wrapper):${NC}"
    echo -e "   • ${BOLD}${YELLOW}pitrove config${NC}   — Interactive settings wizard"
    echo -e "   • ${BOLD}${YELLOW}pitrove restart${NC}  — Restart the background service"
    echo -e "   • ${BOLD}${YELLOW}pitrove logs${NC}     — Tail rendering logs in real-time"
    echo -e "   • ${BOLD}${YELLOW}pitrove status${NC}   — Check container status"
    echo
    echo -e " ${BOLD}${WHITE}Service Status:${NC}"
    echo -e "   • Systemd unit installed (starts on first launch)"
}
print_success_card

# ── Auto-Start piTrove Background Service & Container ───────────────────────
echo
info "Auto-starting piTrove background service & container..."
systemctl enable piTrove.service &>/dev/null || true
systemctl start piTrove.service &>/dev/null || true

wait_count=0
max_wait=90
info "Waiting for background container to start..."

container_ready=0
while [[ $wait_count -lt $max_wait ]]; do
    container_status=$(docker inspect --format="{{.State.Status}}" piTrove 2>/dev/null || echo "")
    if [[ "$container_status" == "running" || "$container_status" == "healthy" ]]; then
        container_ready=1
        break
    fi
    elapsed=$(( wait_count ))
    printf "
   ${GRAY}▸ %ds elapsed - container initializing...${NC}" "$elapsed"
    sleep 1
    wait_count=$(( wait_count + 1 ))
done

if [[ $container_ready -eq 1 ]]; then
    printf "
[K"
    ok "Container started successfully (status: ${GREEN}running${NC})"
else
    printf "
[K"
    warn "Container initialization timed out, but systemd service is active in background"
fi

# ── Next Steps Prompt ─────────────────────────────────────────────────────────
echo
echo -e " ${BOLD}${CYAN}What would you like to do next?${NC}"
echo
echo -e " ${BOLD}${GREEN}[1]${NC} Launch First-Time Configuration Wizard now"
echo -e " ${BOLD}${GRAY}[2]${NC} Exit to terminal (piTrove is running in background)"
echo
echo -n -e " ▸ Choice [1/2]: "
safe_read -r next_action || true
next_action="${next_action:-1}"

if [[ "$next_action" == "1" ]]; then
    echo
    if [[ $container_ready -eq 0 ]]; then
        container_status=$(docker inspect --format="{{.State.Status}}" piTrove 2>/dev/null || echo "")
        if [[ "$container_status" == "running" || "$container_status" == "healthy" ]]; then
            container_ready=1
        fi
    fi

    if [[ $container_ready -eq 1 ]]; then
        info "Launching 11-category interactive terminal config wizard..."
        if [[ ! -t 0 ]] && { true < /dev/tty; } 2>/dev/null; then
            exec 0< /dev/tty
        fi
        docker exec -it piTrove /app/piTrove --config-wizard /app/config/config.toml || G_EXIT_CODE=2
        stty sane 2>/dev/null || true
        tput cnorm 2>/dev/null || true
        echo
    else
        warn "Container is still initializing. Launch wizard manually once ready with: ${CYAN}pitrove config${NC}"
    fi
else
    echo
    ok "Exited to terminal. piTrove is running in background."
    info "You can access the config wizard anytime with: ${CYAN}pitrove config${NC}"
fi

stty sane 2>/dev/null || true
tput cnorm 2>/dev/null || true
echo
ok "Installation & setup completed cleanly."
exit 0


# ── Ordered Next-Steps Checklist ────────────────────────────────────────────────
