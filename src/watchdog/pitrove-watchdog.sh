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

# NAS mount is usable: attached AND readable within 5s (CIFS session alive).
# A dead CIFS session can leave the mount attached while reads hang (hard
# mount), so a plain mount check is not enough — the timed read catches it.
nas_is_healthy() {
    mountpoint -q "$CIFS_MOUNT" 2>/dev/null || return 1
    timeout 5 ls "$CIFS_MOUNT" >/dev/null 2>&1
}

# App container reports healthy (fresh heartbeat)
container_is_healthy() {
    docker inspect --format='{{.State.Health.Status}}' "$DOCKER_CONTAINER" 2>/dev/null | grep -q "healthy"
}

# Detect if Docker container has a stale/empty bind-mount while the host NAS is healthy.
container_bind_is_stale() {
    docker inspect --format='{{.State.Running}}' "$DOCKER_CONTAINER" 2>/dev/null | grep -q "true" || return 1
    if nas_is_healthy && [ -n "$(ls -A "$CIFS_MOUNT" 2>/dev/null)" ]; then
        if ! docker exec "$DOCKER_CONTAINER" /bin/bash -c "test -n \"\$(ls -A /app/media 2>/dev/null)\"" 2>/dev/null; then
            return 0
        fi
    fi
    return 1
}

# Force-refresh the fstab-managed CIFS mount. Detach first (force, then lazy)
# before remounting.
refresh_nas_mount() {
    log "Refreshing network storage mount at $CIFS_MOUNT..."
    umount -f "$CIFS_MOUNT" 2>/dev/null || umount -l "$CIFS_MOUNT" 2>/dev/null || true
    sleep 1
    mount -a 2>/dev/null || true
    if nas_is_healthy; then
        log "Network storage mount at $CIFS_MOUNT is mounted and readable."
        return 0
    else
        if mountpoint -q "$CIFS_MOUNT"; then
            log "WARNING: $CIFS_MOUNT mounted but unreadable after refresh"
        else
            log "WARNING: $CIFS_MOUNT not mounted after refresh"
        fi
        return 1
    fi
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
        current_time=$(date +%s)

        if [ "$WAS_OFFLINE" = true ]; then
            # Check cooldown throttle
            if [ $((current_time - LAST_REMOUNT)) -ge "$CIFS_REMOUNT_COOLDOWN" ]; then
                if container_is_healthy && nas_is_healthy && ! container_bind_is_stale; then
                    # Brief WiFi blip: CIFS session survived, container healthy, and media mount intact.
                    log "Network recovered; app healthy and NAS readable - no action needed (self-recovered)."
                    WAS_OFFLINE=false
                else
                    LAST_REMOUNT=$current_time
                    log "Network connection recovered. Performing recovery sequence..."
                    refresh_nas_mount
                    if nas_is_healthy; then
                        log "NAS mounted successfully. Restarting application systemd service..."
                        systemctl reset-failed piTrove.service 2>/dev/null || true
                        systemctl restart piTrove.service 2>/dev/null || true
                        WAS_OFFLINE=false
                    else
                        log "WARNING: NAS not yet reachable/mounted after network recovery. Deferring application restart."
                    fi
                fi
            fi
        fi

        # Verify piTrove application service & container health when network is online
        if ! systemctl is-active --quiet piTrove.service 2>/dev/null; then
            log "Service check: piTrove.service is inactive! Refreshing NAS mount and auto-reviving service..."
            refresh_nas_mount
            systemctl reset-failed piTrove.service 2>/dev/null || true
            systemctl restart piTrove.service 2>/dev/null || true
        elif docker inspect --format='{{.State.Health.Status}}' "$DOCKER_CONTAINER" 2>/dev/null | grep -q "unhealthy"; then
            log "Health check: piTrove container reported UNHEALTHY! Refreshing NAS mount and restarting service..."
            refresh_nas_mount
            systemctl reset-failed piTrove.service 2>/dev/null || true
            systemctl restart piTrove.service 2>/dev/null || true
        elif ! nas_is_healthy; then
            if [ $((current_time - LAST_REMOUNT)) -ge "$CIFS_REMOUNT_COOLDOWN" ]; then
                LAST_REMOUNT=$current_time
                log "NAS health check: $CIFS_MOUNT is unmounted or unreadable! Attempting refresh..."
                refresh_nas_mount
                if nas_is_healthy; then
                    log "NAS recovered successfully! Restarting piTrove to re-bind media mount..."
                    systemctl reset-failed piTrove.service 2>/dev/null || true
                    systemctl restart piTrove.service 2>/dev/null || true
                    WAS_OFFLINE=false
                else
                    log "NAS check: $CIFS_MOUNT still unavailable. Retrying after cooldown."
                fi
            fi
        elif container_bind_is_stale; then
            if [ $((current_time - LAST_REMOUNT)) -ge "$CIFS_REMOUNT_COOLDOWN" ]; then
                LAST_REMOUNT=$current_time
                log "Stale mount check: host $CIFS_MOUNT is mounted but container /app/media is empty! Restarting piTrove..."
                systemctl reset-failed piTrove.service 2>/dev/null || true
                systemctl restart piTrove.service 2>/dev/null || true
            fi
        fi
        FAIL_COUNT=0
        WIFI_RESET_DONE=false
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        log "Network check failed ($FAIL_COUNT/$MAX_FAIL)"
        if [ "$FAIL_COUNT" -ge 3 ]; then
            WAS_OFFLINE=true
        fi

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
