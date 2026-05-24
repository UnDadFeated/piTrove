#!/usr/bin/env bash
# install.sh — piTrove v11.1.5 Premium Graphical Installer
# Target: Debian Trixie (13) 64-bit on Raspberry Pi 4/5

set -eo pipefail

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

draw_line() {
    local char=${1:-"═"}
    local len=${2:-60}
    for ((i=0; i<len; i++)); do echo -n "$char"; done
}

banner() {
    clear
    echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║${NC}  ${BOLD}${WHITE}                  piTrove v11.1.5 Installation               ${NC}  ${CYAN}║${NC}"
    echo -e "${CYAN}║${NC}  ${MAGENTA}               The Ultra-Premium Picture Frame              ${NC}  ${CYAN}║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
    echo
}

info()  { echo -e "   ${CYAN}[ ℹ ]${NC}  $*"; }
warn()  { echo -e "   ${YELLOW}[ ⚠ ]${NC}  $*"; }
ok()    { echo -e "   ${GREEN}[ ✓ ]${NC}  $*"; }

fail()  { 
    echo
    echo -e "${RED}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║${NC}  ${BOLD}${RED}[ ✘ ] ERROR: $*${NC}"
    echo -e "${RED}╚══════════════════════════════════════════════════════════════╝${NC}"
    echo
    exit 1
}

yesno() {
    echo -e -n "\n   ${BOLD}${YELLOW}▸ $* [y/N]:${NC} "
    read -r resp 
    [[ "$resp" == [yY]* ]]
}

# ── Dynamic Spinner (Braille Unicode Animation) ─────────────────────────────────
show_spinner() {
    local pid=$1
    local label="$2"
    local delay=0.08
    local spin_chars=("⠋" "⠙" "⠹" "⠸" "⠼" "⠴" "⠦" "⠧" "⠇" "⠏")
    local i=0
    
    tput civis 2>/dev/null || echo -ne "\033[?25l"
    
    while kill -0 "$pid" 2>/dev/null; do
        local char="${spin_chars[i]}"
        printf "\r   ${CYAN}[%s]${NC}  %s... " "$char" "$label"
        i=$(( (i + 1) % 10 ))
        sleep $delay
    done
    
    tput cnorm 2>/dev/null || echo -ne "\033[?25h"
    printf "\r                                                                                \r"
}

run_with_spinner() {
    local label="$1"
    shift
    local log_file="/tmp/pitrove_cmd_$((100 + RANDOM % 900)).log"
    
    "$@" > "$log_file" 2>&1 &
    local pid=$!
    
    show_spinner "$pid" "$label"
    
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
}

# ── Compilation Progress Bar Monitor ───────────────────────────────────────────
run_compilation_with_progress() {
    local build_dir="$1"
    local log_file="/tmp/pitrove_compile.log"
    rm -f "$log_file"
    
    cd "$build_dir"
    
    # Run cmake generation
    cmake -B build -DCMAKE_BUILD_TYPE=Release > "$log_file" 2>&1 &
    local cmake_pid=$!
    show_spinner "$cmake_pid" "Configuring CMake build system"
    wait "$cmake_pid"
    local status=$?
    if [[ "$status" -ne 0 ]]; then
        echo -e "   ${RED}[ ✘ ]  CMake configuration failed!${NC}"
        echo -e "   ${YELLOW}─────── CMAKE LOG: ───────${NC}"
        tail -n 20 "$log_file" | sed 's/^/   /'
        fail "CMake configuration failed."
    fi
    
    # Start compilation in background
    cmake --build build -j3 > "$log_file" 2>&1 &
    local pid=$!
    
    local last_percent=0
    tput civis 2>/dev/null || echo -ne "\033[?25l"
    
    while kill -0 "$pid" 2>/dev/null; do
        # Extract the last percent from log file
        local percent=$(grep -o -E "\[[ 0-9]{1,3}%\]" "$log_file" 2>/dev/null | tail -n 1 | tr -d '[]% ' || echo "")
        if [[ -n "$percent" && "$percent" =~ ^[0-9]+$ ]]; then
            last_percent="$percent"
        fi
        
        # Draw the progress bar
        local width=40
        local completed=$(( last_percent * width / 100 ))
        local remaining=$(( width - completed ))
        
        local bar_filled=""
        for ((k=0; k<completed; k++)); do bar_filled="${bar_filled}█"; done
        local bar_empty=""
        for ((k=0; k<remaining; k++)); do bar_empty="${bar_empty}░"; done
        
        printf "\r   ${MAGENTA}[%3d%%]${NC} [${GREEN}%s${NC}${DARK_GRAY}%s${NC}] Compiling piTrove core..." "$last_percent" "$bar_filled" "$bar_empty"
        sleep 0.15
    done
    
    wait "$pid"
    local status=$?
    
    tput cnorm 2>/dev/null || echo -ne "\033[?25h"
    printf "\r                                                                                    \r"
    
    if [[ "$status" -ne 0 ]]; then
        echo -e "   ${RED}[ ✘ ]  piTrove compilation failed!${NC}"
        echo -e "   ${YELLOW}─────── COMPILATION ERRORS: ───────${NC}"
        grep -E "error:|warning:" "$log_file" | tail -n 20 | sed 's/^/   /' || tail -n 20 "$log_file" | sed 's/^/   /'
        echo -e "   ${YELLOW}───────────────────────────────────${NC}"
        fail "piTrove build failed. Check ${log_file} for full details."
    else
        # 100% complete bar
        local width=40
        local bar_filled=""
        for ((k=0; k<width; k++)); do bar_filled="${bar_filled}█"; done
        printf "   ${GREEN}[100%%] [${GREEN}%s${NC}] piTrove compilation completed!${NC}\n" "$bar_filled"
        rm -f "$log_file"
    fi
}

# ── Render Initial Screen ─────────────────────────────────────────────────────
banner

# 1. Detect primary user (fallback if UID 1000 is modified)
PRIMARY_USER=$(getent passwd 1000 | cut -d: -f1 || true)
if [[ -z "$PRIMARY_USER" ]]; then
    PRIMARY_USER=$(logname 2>/dev/null || echo $USER)
fi

if [[ "$PRIMARY_USER" == "root" ]]; then
    fail "Installer detected 'root'. Please run from a standard user account with sudo."
fi
PRIMARY_HOME="/home/$PRIMARY_USER"
info "Primary user: ${BOLD}${WHITE}$PRIMARY_USER${NC} (${CYAN}$PRIMARY_HOME${NC})"

# 2. Bootstrap packages (git, lsb_release, pkg-config, curl needed below)
run_with_spinner "Updating system package repositories" apt-get update -qq
run_with_spinner "Installing bootstrap tools" apt-get install -y -qq git curl lsb-release pkg-config

# Bootstrap Docker
if ! command -v docker &>/dev/null; then
    run_with_spinner "Installing Docker Engine" sh -c "curl -fsSL https://get.docker.com | sh"
fi
if ! command -v docker compose &>/dev/null; then
    run_with_spinner "Installing Docker Compose Plugin" apt-get install -y -qq docker-compose-plugin
fi

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
ok "Debian Trixie 64-bit validated successfully"

# 4. Root check
if [[ "$(id -u)" -ne 0 ]]; then
    fail "This script must be run as root (sudo)"
fi

# 5. Kill conflicting display servers
for svc in labwc-tty1 seatd; do
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
        run_with_spinner "Stopping conflicting display service ($svc)" systemctl stop "$svc"
        run_with_spinner "Disabling conflicting display service ($svc)" systemctl disable "$svc"
    fi
done

# 5a. Kill existing processes and stop services
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

# 5b. Safeguard SSH
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

# 6. Disk space check
AVAIL=$(df --output=avail / | tail -n 1 | tr -d ' ')
if [[ "$AVAIL" -lt 5242880 ]]; then
    warn "Low disk space: Only $((AVAIL / 1024))MB free on / (recommended: 5GB for cache/compiling)"
else
    ok "Disk space check passed ($((AVAIL / 1024 / 1024))GB available)"
fi

# ── Install comprehensive system packages ──────────────────────────────────────
run_with_spinner "Installing host system dependencies (cifs-utils, git)" apt-get install -y -qq \
    git curl cifs-utils

# ── DRM and Docker group configuration ─────────────────────────────────────────
run_with_spinner "Adding $PRIMARY_USER to video, render, and docker groups for hardware & docker permission" usermod -aG video,render,docker "$PRIMARY_USER"

# ── DRM/KMS firmware configuration (Pi 4/5) ───────────────────────────────────
BOOT_CFG="/boot/firmware/config.txt"
if [[ -f "$BOOT_CFG" ]]; then
    if ! grep -q "dtoverlay=vc4-kms-v3d" "$BOOT_CFG"; then
        echo "" >> "$BOOT_CFG"
        echo "# piTrove DRM/KMS Display Configuration" >> "$BOOT_CFG"
        echo "dtoverlay=vc4-kms-v3d,cma-256" >> "$BOOT_CFG"
        echo "gpu_mem=128" >> "$BOOT_CFG"
        ok "Configured vc4-kms-v3d overlay & gpu_mem in $BOOT_CFG"
    else
        ok "vc4-kms-v3d already configured in $BOOT_CFG"
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
    # Restart NetworkManager in background to prevent installer SSH disconnection dropouts
    systemctl restart NetworkManager &>/dev/null &
    ok "Disabled NetworkManager Wi-Fi Power Saving persistently"
else
    ok "NetworkManager not active, skipping power-saving overrides"
fi

# ── Storage Selection Dialog ───────────────────────────────────────────────────
echo
echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║${NC}  ${BOLD}${WHITE}               SELECT STORAGE MODE                          ${NC}  ${CYAN}║${NC}"
echo -e "${CYAN}╠══════════════════════════════════════════════════════════════╣${NC}"
echo -e "${CYAN}║${NC}  ${BOLD}${GREEN}1)${NC} ${WHITE}NAS (SMB/CIFS Network Share)${NC}                          ${CYAN}║${NC}"
echo -e "${CYAN}║${NC}     - Mounts your remote server's archive to /mnt/nas        ${CYAN}║${NC}"
echo -e "${CYAN}║${NC}                                                              ${CYAN}║${NC}"
echo -e "${CYAN}║${NC}  ${BOLD}${GREEN}2)${NC} ${WHITE}Local Drive (USB / MicroSD)${NC}                           ${CYAN}║${NC}"
echo -e "${CYAN}║${NC}     - Keeps all assets stored locally on the Pi             ${CYAN}║${NC}"
echo -e "${CYAN}║${NC}                                                              ${CYAN}║${NC}"
echo -e "${CYAN}║${NC}  ${BOLD}${GREEN}3)${NC} ${WHITE}Other Network Drive (NFS/Custom)${NC}                         ${CYAN}║${NC}"
echo -e "${CYAN}║${NC}     - Custom setup options for NFS or other mounts           ${CYAN}║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
echo
if [[ -n "$STORAGE_CHOICE" ]]; then
    storage_choice="$STORAGE_CHOICE"
    info "Using storage choice from environment: $storage_choice"
else
    echo -n -e "   ${BOLD}${YELLOW}▸ Enter your choice [1-3]:${NC} "
    read -r storage_choice
fi

USE_NAS=0
SHARE_IP=""
SHARE_MOUNT="/mnt/nas"
SHARE_PATH=""
SHARE_PROTOCOL="cifs"

case "$storage_choice" in
    1)
        USE_NAS=1
        if [[ -n "$NAS_IP" ]]; then
            SHARE_IP="$NAS_IP"
            info "Using NAS IP from environment: $SHARE_IP"
        else
            echo -n -e "\n   ${BOLD}${CYAN}▸ NAS IP Address:${NC} "
            read -r SHARE_IP
        fi
        if [[ -z "$SHARE_IP" ]] || ! echo "$SHARE_IP" | grep -qE '^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$'; then
            fail "Invalid IP format: '$SHARE_IP'. Must be dotted-quad (e.g. 192.168.4.111)"
        fi
        if [[ -n "$NAS_SHARE" ]]; then
            SHARE_PATH="$NAS_SHARE"
            info "Using NAS share path from environment: $SHARE_PATH"
        else
            echo -n -e "   ${BOLD}${CYAN}▸ Share path [default: /Home/Archive]:${NC} "
            read -r SHARE_PATH
        fi
        SHARE_PATH="${SHARE_PATH:-/Home/Archive}"
        SHARE_PROTOCOL="cifs"
        ;;
    2)
        info "Local drive mode selected. Default local mount point: ${BOLD}/mnt/media${NC}"
        SHARE_MOUNT="/mnt/media"
        mkdir -p "$SHARE_MOUNT"
        ok "Local media directory initialized at $SHARE_MOUNT"
        ;;
    3)
        echo
        echo -e "   ${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
        echo -e "   ${CYAN}║${NC}  ${BOLD}${WHITE}               SELECT PROTOCOL                              ${NC}  ${CYAN}║${NC}"
        echo -e "   ${CYAN}╠══════════════════════════════════════════════════════════════╣${NC}"
        echo -e "   ${CYAN}║${NC}  ${BOLD}${GREEN}a)${NC} ${WHITE}SMB/CIFS Network Share${NC}                                ${CYAN}║${NC}"
        echo -e "   ${CYAN}║${NC}  ${BOLD}${GREEN}b)${NC} ${WHITE}NFS Network Share${NC}                                     ${CYAN}║${NC}"
        echo -e "   ${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
        echo
        echo -n -e "      ${BOLD}${YELLOW}▸ Select protocol [a-b]:${NC} "
        read -r proto_choice 
        case "$proto_choice" in
            a)
                SHARE_PROTOCOL="cifs"
                echo -n -e "\n      ${BOLD}${CYAN}▸ SMB Server IP Address:${NC} "
                read -r SHARE_IP 
                echo -n -e "      ${BOLD}${CYAN}▸ SMB Share path [default: /Shared]:${NC} "
                read -r SHARE_PATH 
                SHARE_PATH="${SHARE_PATH:-/Shared}"
                ;;
            b)
                SHARE_PROTOCOL="nfs"
                echo -n -e "\n      ${BOLD}${CYAN}▸ NFS Server IP Address [default: 192.168.4.111]:${NC} "
                read -r SHARE_IP 
                SHARE_IP="${SHARE_IP:-192.168.4.111}"
                echo -n -e "      ${BOLD}${CYAN}▸ NFS Export path [default: /mnt/nas]:${NC} "
                read -r SHARE_PATH 
                SHARE_PATH="${SHARE_PATH:-/mnt/nas}"
                ;;
            *)
                fail "Invalid protocol choice"
                ;;
        esac
        ;;
    *)
        fail "Invalid storage selection"
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
                read -r nas_user 
                echo -n -e "   ${BOLD}${YELLOW}▸ Password:${NC} "
                read -rs nas_pass 
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
                read -r nas_user 
                echo -n -e "   ${BOLD}${YELLOW}▸ Password:${NC} "
                read -rs nas_pass 
                echo
                printf 'username=%s\npassword=%s\n' "$nas_user" "$nas_pass" > "$CRED_FILE"
                chmod 600 "$CRED_FILE"
                chown $PRIMARY_USER:$PRIMARY_USER "$CRED_FILE"
            fi
        fi
    fi

    # Format fstab configuration row
    if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
        FSTAB_LINE="# piTrove Network Share
//$SHARE_IP/${SHARE_PATH#/} $SHARE_MOUNT cifs credentials=$PRIMARY_HOME/nas.cred,ro,uid=1000,gid=1000,vers=3.0,_netdev,nofail 0 0"
    elif [[ "$SHARE_PROTOCOL" == "nfs" ]]; then
        FSTAB_LINE="# piTrove Network Share
$SHARE_IP:$SHARE_PATH $SHARE_MOUNT $SHARE_PROTOCOL defaults,_netdev,timeo=10,retrans=3,nofail 0 0"
    fi

    # Clean old entries
    sed -i '/# piTrove /d' /etc/fstab
    sed -i '/# PiTrove /d' /etc/fstab
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
                if [[ -d "$SHARE_MOUNT" ]] && ls "$SHARE_MOUNT" >/dev/null 2>&1; then
                    info "First few files detected:"
                    ls "$SHARE_MOUNT" 2>/dev/null | head -5 | sed 's/^/     • /'
                fi
                MOUNT_OK=1
                NAS_MOUNT_SUCCESS=1
                break
            fi

            warn "Mount failed (attempt $MOUNT_ATTEMPTS/3)"
            echo
            if [[ "$MOUNT_ATTEMPTS" -eq 3 ]]; then
                warn "Cannot mount network share automatically."
                echo -e "   ${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
                echo -e "   ${CYAN}║${NC}  ${BOLD}${WHITE}               MOUNT FAIL OPTIONS                           ${NC}  ${CYAN}║${NC}"
                echo -e "   ${CYAN}╠══════════════════════════════════════════════════════════════╣${NC}"
                echo -e "   ${CYAN}║${NC}  ${BOLD}${GREEN}1)${NC} Retry connection                                          ${CYAN}║${NC}"
                echo -e "   ${CYAN}║${NC}  ${BOLD}${GREEN}2)${NC} Re-enter path configuration                                ${CYAN}║${NC}"
                echo -e "   ${CYAN}║${NC}  ${BOLD}${GREEN}3)${NC} Re-enter username & password                               ${CYAN}║${NC}"
                echo -e "   ${CYAN}║${NC}  ${BOLD}${GREEN}4)${NC} Skip and mount manually later                              ${CYAN}║${NC}"
                echo -e "   ${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
                echo
                echo -n -e "      ${BOLD}${YELLOW}▸ Choose option [1-4]:${NC} "
                read -r mount_opt 
                case "$mount_opt" in
                    1) MOUNT_ATTEMPTS=0; continue ;;
                    2)
                        echo -n -e "      ${BOLD}${CYAN}▸ NAS IP [$SHARE_IP]:${NC} "
                        read -r _tmp ; SHARE_IP="${_tmp:-$SHARE_IP}"
                        echo -n -e "      ${BOLD}${CYAN}▸ Share Path [$SHARE_PATH]:${NC} "
                        read -r _tmp ; SHARE_PATH="${_tmp:-$SHARE_PATH}"
                        if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
                            FSTAB_LINE="# piTrove Network Share
//$SHARE_IP/${SHARE_PATH#/} $SHARE_MOUNT cifs credentials=$PRIMARY_HOME/nas.cred,ro,uid=1000,gid=1000,vers=3.0,_netdev,nofail 0 0"
                        else
                            FSTAB_LINE="# piTrove Network Share
$SHARE_IP:$SHARE_PATH $SHARE_MOUNT $SHARE_PROTOCOL defaults,_netdev,timeo=10,retrans=3,nofail 0 0"
                        fi
                        sed -i '/# piTrove /d' /etc/fstab
                        echo "$FSTAB_LINE" >> /etc/fstab
                        systemctl daemon-reload 2>/dev/null || true
                        MOUNT_ATTEMPTS=0
                        continue
                        ;;
                    3)
                        if [[ "$SHARE_PROTOCOL" == "cifs" ]]; then
                            echo -n -e "      ${BOLD}${YELLOW}▸ Username:${NC} "
                            read -r nas_user 
                            echo -n -e "      ${BOLD}${YELLOW}▸ Password:${NC} "
                            read -rs nas_pass 
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
                        break
                        ;;
                    *)
                        warn "Invalid option. Skipping..."
                        break
                        ;;
                esac
            fi
        done
    fi
fi

# ── Clone / Update Git Repository ──────────────────────────────────────────────
info "Setting up piTrove repository clone..."
if [[ ! -d "$PRIMARY_HOME/piTrove/.git" ]]; then
    run_with_spinner "Cloning piTrove production repository" git clone https://github.com/UnDadFeated/piTrove.git "$PRIMARY_HOME/piTrove"
else
    info "Repository exists. Updating source..."
    cd "$PRIMARY_HOME/piTrove"
    git pull || warn "Repository update failed. Using active local copy."
fi
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove"

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

# ── Build piTrove Docker Container ─────────────────────────────────────────────
info "Building piTrove Docker Container (compiling binary inside container)..."
cd "$PRIMARY_HOME/piTrove"

# Write local .env file first for docker-compose to use during build/run
echo "SDL_VIDEO_KMSDRM_DEVICE=/dev/dri/$PROBED_CARD" > .env
echo "SDL_KMSDRM_DEVICE_INDEX=$PROBED_INDEX" >> .env
echo "MEDIA_DIR=$SHARE_MOUNT" >> .env
chown $PRIMARY_USER:$PRIMARY_USER .env

run_with_spinner "Building piTrove container image" docker compose build
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/.docker" 2>/dev/null || true

# ── Create Directories ────────────────────────────────────────────────────────
info "Creating internal directory tree..."
mkdir -p "$PRIMARY_HOME/piTrove/cache" "$PRIMARY_HOME/piTrove/config"
mkdir -p "$PRIMARY_HOME/piTrove/src/config"
mkdir -p "$PRIMARY_HOME/piTrove/logs"
mkdir -p "$PRIMARY_HOME/piTrove/subtitles"
mkdir -p "$PRIMARY_HOME/piTrove/src/fonts"

# ── Scan Window Setup ──────────────────────────────────────────────────────────
echo
if [[ -n "$SCAN_WINDOW_DAYS" ]]; then
    scan_input="$SCAN_WINDOW_DAYS"
    info "Using scan window days from environment: $scan_input"
else
    echo -n -e "   ${BOLD}${YELLOW}▸ Temporal window (current month +/- days, 0=disable) [default: 5]:${NC} "
    read -r scan_input
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

# ── Configuration TOML ─────────────────────────────────────────────────────────
info "Writing configuration options..."
CONFIG_FILE="$PRIMARY_HOME/piTrove/config/config.toml"

if [[ -f "$CONFIG_FILE" ]]; then
    warn "Existing config.toml detected. Preserving custom user settings..."
    cp "$CONFIG_FILE" "$CONFIG_FILE.bak.$(date +%Y%m%d%H%M%S)"
    # Check if [mqtt] section is already in the file. If not, append it!
    if ! grep -q "\[mqtt\]" "$CONFIG_FILE"; then
        info "Upgrading existing config.toml with new MQTT options..."
        cat >> "$CONFIG_FILE" <<EOF

[mqtt]
enabled = 0
broker = "192.168.4.111"
port = 1883
user = ""
pass = ""
topic_prefix = "piTrove"
motionsensor_topic = "home/motionsensor"
motionsensor_cooldown = 60
EOF
    fi
else
    cat > "$CONFIG_FILE" <<EOF
# ==========================================
# piTrove Configuration File (v11.1.5)
# ==========================================

[paths]
media_dir = "/app/media"
cache_dir = "/app/cache"
log_dir = "/app/logs"

[display]
rotation = 0
splash_file = "src/splash.png"
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
weather_lat = -999.0
weather_lon = -999.0

[remote]
http_enabled = 0
http_port = 8080

[mqtt]
enabled = 0
broker = "192.168.4.111"
port = 1883
user = ""
pass = ""
topic_prefix = "piTrove"
motionsensor_topic = "home/motionsensor"
motionsensor_cooldown = 60
EOF
fi

chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove/config"
ok "Default production config.toml generated"

# Double check ownerships
chown -R $PRIMARY_USER:$PRIMARY_USER "$PRIMARY_HOME/piTrove"

# ── systemd Service Deployment ─────────────────────────────────────────────────
info "Installing daemon background service..."

# Probe active DRM card
PROBED_CARD=$(find /sys/class/drm/ -name "card*-*" -exec grep -q "^connected$" {}/status \; -print -quit | sed -E 's|.*/(card[0-9]+)-.*|\1|')
if [ -z "$PROBED_CARD" ]; then
    PROBED_CARD=$(find /sys/class/drm/ -name "card[0-9]" -print -quit | sed -E 's|.*/(card[0-9]+)|\1|')
fi
if [ -z "$PROBED_CARD" ]; then
    PROBED_CARD="card1"
fi
PROBED_INDEX=${PROBED_CARD#card}

info "Systemd: Using probed DRM GPU device /dev/dri/$PROBED_CARD (index $PROBED_INDEX)"

# Sourcing correct KMSDRM parameters for stable SDL video playback
cat > /etc/systemd/system/piTrove.service <<EOF
[Unit]
Description=PiTrove Docker Digital Picture Frame
After=multi-user.target network-online.target docker.service
Wants=network-online.target docker.service
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=simple
User=$PRIMARY_USER
Group=$PRIMARY_USER
WorkingDirectory=$PRIMARY_HOME/piTrove
ExecStartPre=-/usr/bin/docker compose down
ExecStart=/usr/bin/docker compose up --force-recreate
ExecStop=/usr/bin/docker compose down
Restart=always
RestartSec=15
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable piTrove.service &>/dev/null
systemctl start piTrove.service &>/dev/null || true
ok "piTrove.service successfully registered, enabled & started"

# ── Cleanup Bootstrap File ─────────────────────────────────────────────────────
BOOTSTRAP="$PRIMARY_HOME/install.sh"
if [[ -f "$BOOTSTRAP" ]]; then
    rm -f "$BOOTSTRAP"
    info "Removed temporary bootstrap copy"
fi

# ── Successful Completion Dashboard ───────────────────────────────────────────
clear
banner

print_success_card() {
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║${NC}  ${BOLD}${GREEN}✔  INSTALLATION COMPLETED SUCCESSFULLY!                     ${NC}  ${GREEN}║${NC}"
    echo -e "${GREEN}╠══════════════════════════════════════════════════════════════╣${NC}"
    echo -e "${GREEN}║${NC}  ${BOLD}${WHITE}Path Locations:${NC}                                             ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}   • Container Base:  ${CYAN}$PRIMARY_HOME/piTrove/${NC}                    ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}   • Configuration:   ${CYAN}config/config.toml${NC}                      ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}   • SQLite Cache:    ${CYAN}cache/cache.db${NC}                          ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}   • Service Logs:    ${CYAN}logs/piTrove_*.log${NC}                      ${GREEN}║${NC}"
    
    # Probe active IP address
    local IP_ADDR
    IP_ADDR=$(hostname -I | awk '{print $1}' || echo "127.0.0.1")
    if [[ -z "$IP_ADDR" ]]; then
        IP_ADDR="127.0.0.1"
    fi
    local url="http://${IP_ADDR}:8080/"
    local text="   • URL: $url"
    local pad=$(( 60 - ${#text} ))
    local spaces=""
    if [[ $pad -gt 0 ]]; then
        spaces=$(printf '%*s' "$pad" "")
    fi

    echo -e "${GREEN}║${NC}                                                              ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}  ${BOLD}${WHITE}Web Remote Dashboard & MQTT HUD URL:${NC}                        ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}${BOLD}${CYAN}${text}${NC}${spaces}${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}     Click to view MQTT telemetry, control the screen physically, ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}     and trigger motion simulation sweeps remotely.        ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}                                                              ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}  ${BOLD}${WHITE}How to Manage & Control:${NC}                                    ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}   • ${BOLD}${YELLOW}docker compose exec -it pitrove /app/piTrove --config /app/config/config.toml${NC} ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}                        Runs the 8-tab interactive settings    ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}                        wizard in your terminal console.       ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}   • ${BOLD}${YELLOW}sudo systemctl restart piTrove.service${NC}                   ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}                        Reboots the background daemon container ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}                        safely if configuration is modified.   ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}                                                              ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}  ${BOLD}${WHITE}Service Status:${NC}                                             ${GREEN}║${NC}"
    echo -e "${GREEN}║${NC}   • Systemd unit is installed and set to launch on boot.     ${GREEN}║${NC}"
    
    if [[ "$storage_choice" -eq 2 ]]; then
        echo -e "${GREEN}║${NC}   • Storage: ${CYAN}Local drive mode enabled.${NC}                      ${GREEN}║${NC}"
    elif [[ "$USE_NAS" -eq 1 || "$storage_choice" == "3" ]]; then
        if [[ "$NAS_MOUNT_SUCCESS" -eq 1 ]]; then
            echo -e "${GREEN}║${NC}   • Storage: ${CYAN}NAS Share successfully mounted at $SHARE_MOUNT.${NC}    ${GREEN}║${NC}"
        else
            echo -e "${GREEN}║${NC}   • ${RED}Storage Warning: NAS mount failed.${NC}                       ${GREEN}║${NC}"
            echo -e "${GREEN}║${NC}     Manually add to /etc/fstab and run 'sudo mount -a'       ${GREEN}║${NC}"
        fi
    fi
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════════╝${NC}"
}

print_success_card
echo
