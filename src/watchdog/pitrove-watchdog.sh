#!/usr/bin/env bash
# piTrove Network Watchdog - External system-level monitor
# Installed by install.sh to /usr/local/bin/pitrove-watchdog.sh
# Runs as a systemd service independent of Docker.
# Checks network health every 15s, attempts WiFi reset, then clean reboot if needed.
set -uo pipefail

GATEWAY="${GATEWAY:-192.168.4.1}"
INTERFACE="${INTERFACE:-wlan0}"
CIFS_MOUNT="${CIFS_MOUNT:-/mnt/nas}"
DOCKER_CONTAINER="${DOCKER_CONTAINER:-piTrove}"
CIFS_REMOUNT_COOLDOWN="${CIFS_REMOUNT_COOLDOWN:-60}"

FAIL_COUNT=0
MAX_FAIL=12          # 12 checks * 15 seconds = 180s (3 minutes) offline trigger
WIFI_RESET_DONE=false
WAS_OFFLINE=false
LAST_REMOUNT=0

# Source optional config override (installed by install.sh)
CONF_FILE="/etc/pitrove/wdog.conf"
if [ -f "$CONF_FILE" ]; then
    # shellcheck source=/dev/null
    source "$CONF_FILE"
fi

# If disabled by flag file, exit silently
if [ -f /etc/pitrove/wdog-disable ]; then
    exit 0
fi

log() {
    if command -v logger >/dev/null 2>&1; then
        logger -t "pitrove-watchdog" "$@"
    fi
    
    # Also write to local log file in logs/ folder if directory exists
    LOG_FILE="${LOG_FILE:-/home/pi/piTrove/logs/pitrove-watchdog.log}"
    LOG_DIR=$(dirname "$LOG_FILE")
    if [ -d "$LOG_DIR" ]; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') pitrove-watchdog: $*" >> "$LOG_FILE" || true
    fi
}

network_is_ok() {
    # Check 1: Does interface exist and have carrier?
    if ip link show "$INTERFACE" 2>/dev/null | grep -q "state UP"; then
        # Check 2: Default route exists
        if ip route | grep -q "^default"; then
            # Check 3: Gateway reachable
            if ping -c 1 -W 3 "$GATEWAY" >/dev/null 2>&1; then
                return 0
            fi
        fi
    fi

    # Fallback: if eth0 is up with default route, network is fine
    if ip link show eth0 2>/dev/null | grep -q "state UP"; then
        if ip route | grep -q "^default.*eth0"; then
            return 0
        fi
    fi

    return 1
}

reset_wifi() {
    log "Attempting soft WiFi reset (wpa_cli reconfigure + interface bounce)..."
    wpa_cli reconfigure 2>/dev/null || true
    ip link set "$INTERFACE" down 2>/dev/null || true
    sleep 2
    ip link set "$INTERFACE" up 2>/dev/null || true
    # Give it time to reassociate
    sleep 15
}

do_reboot() {
    log "CRITICAL: Network offline for 30+ seconds (after WiFi reset if attempted). Rebooting..."
    sync
    # Use clean systemd reboot — ensures proper service shutdown and network initialization on boot
    shutdown -r now
}

# ── Main Loop ───────────────────────────────────────────────────────────────
log "Watchdog started. Monitoring gateway $GATEWAY on $INTERFACE every 15s."

while true; do
    if network_is_ok; then
        if [ "$WAS_OFFLINE" = true ]; then
            current_time=$(date +%s)
            
            # Check cooldown throttle
            if [ $((current_time - LAST_REMOUNT)) -ge "$CIFS_REMOUNT_COOLDOWN" ]; then
                LAST_REMOUNT=$current_time
                log "Network connection recovered. Performing recovery sequence..."
                
                log "Refreshing network storage mount at $CIFS_MOUNT..."
                SYSTEMD_MOUNT=$(systemd-escape -p --suffix=mount "$CIFS_MOUNT")
                systemctl restart "$SYSTEMD_MOUNT" 2>/dev/null || true
                sleep 1
                
                log "Restarting application systemd service..."
                systemctl reset-failed piTrove.service 2>/dev/null || true
                systemctl restart piTrove.service 2>/dev/null || true
            else
                log "Network recovered, but remount/restart is throttled (cooldown active)."
            fi
            WAS_OFFLINE=false
        fi
        # Verify piTrove application service & container health when network is online
        if ! systemctl is-active --quiet piTrove.service 2>/dev/null; then
            log "Service check: piTrove.service is inactive! Auto-reviving service..."
            systemctl reset-failed piTrove.service 2>/dev/null || true
            systemctl restart piTrove.service 2>/dev/null || true
        elif docker inspect --format={{.State.Health.Status}} "" 2>/dev/null | grep -q "unhealthy"; then
            log "Health check: piTrove container reported UNHEALTHY! Restarting service..."
            systemctl reset-failed piTrove.service 2>/dev/null || true
            systemctl restart piTrove.service 2>/dev/null || true
        fi
        FAIL_COUNT=0
        WIFI_RESET_DONE=false
    else
        WAS_OFFLINE=true
        FAIL_COUNT=$((FAIL_COUNT + 1))
        log "Network check failed ($FAIL_COUNT/$MAX_FAIL)"

        if [ "$FAIL_COUNT" -ge "$MAX_FAIL" ]; then
            if [ "$WIFI_RESET_DONE" = false ]; then
                log "Network down for ~3 minutes. Attempting one WiFi reset before reboot..."
                WIFI_RESET_DONE=true
                FAIL_COUNT=0
                reset_wifi
                # Loop back to re-check after the reset wait
                continue
            fi
            do_reboot
        fi
    fi

    sleep 15
done
