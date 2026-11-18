#!/usr/bin/env bash
# piTrove Network Watchdog - External system-level monitor
# Installed by install.sh to /usr/local/bin/pitrove-watchdog.sh
# Runs as a systemd service independent of Docker.
# Checks network health every 15s and attempts soft recovery without rebooting.
set -uo pipefail

GATEWAY="${GATEWAY:-192.168.4.1}"
INTERFACE="${INTERFACE:-wlan0}"
CIFS_MOUNT="${CIFS_MOUNT:-/mnt/nas}"
DOCKER_CONTAINER="${DOCKER_CONTAINER:-piTrove}"
CIFS_REMOUNT_COOLDOWN="${CIFS_REMOUNT_COOLDOWN:-60}"

FAIL_COUNT=0
MAX_FAIL=12          # 12 checks * 15 seconds = 180s (3 minutes) offline trigger
WAS_OFFLINE=false
LAST_RECOVERY=-1


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
    # Primary check: Gateway reachable via any active network interface (wlan0, eth0)
    if ping -c 1 -W 3 "$GATEWAY" >/dev/null 2>&1; then
        return 0
    fi

    # Fallback check: If default route exists and external DNS responds (gateway ICMP blocked)
    if ip route | grep -q "^default"; then
        if ping -c 1 -W 2 1.1.1.1 >/dev/null 2>&1 || ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1; then
            return 0
        fi
    fi

    return 1
}

# NAS mount is usable: attached AND readable within 5s (CIFS session alive).
# A dead CIFS session can leave the mount attached while reads hang (hard
# mount), so a plain mount check is not enough — the timed read catches it.
nas_is_healthy() {
    timeout -k 1 5 mountpoint -q "$CIFS_MOUNT" 2>/dev/null || return 1
    timeout -k 1 5 ls -U "$CIFS_MOUNT" >/dev/null 2>&1
}

container_health() {
    timeout -k 1 5 docker inspect --format='{{.State.Health.Status}}' "$DOCKER_CONTAINER" 2>/dev/null
}

# The caller has already checked that the host NAS is readable.
container_bind_is_stale() {
    timeout -k 1 5 docker inspect --format='{{.State.Running}}' "$DOCKER_CONTAINER" 2>/dev/null | grep -qx true || return 1
    local host_item
    host_item=$(timeout -k 1 5 find "$CIFS_MOUNT" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) || return 1
    [[ -n "$host_item" ]] || return 1
    ! timeout -k 1 5 docker exec "$DOCKER_CONTAINER" /bin/bash -c         'test -n "$(find /app/media -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)"' 2>/dev/null
}

# Refresh only the media mount so recovery cannot mount other fstab entries.
refresh_nas_mount() {
    log "Refreshing network storage mount at $CIFS_MOUNT."
    if timeout -k 1 5 mountpoint -q "$CIFS_MOUNT" 2>/dev/null; then
        timeout -k 1 5 umount -f -- "$CIFS_MOUNT" 2>/dev/null ||
            timeout -k 1 5 umount -l -- "$CIFS_MOUNT" 2>/dev/null || return 1
    fi
    timeout -k 2 15 mount -- "$CIFS_MOUNT" 2>/dev/null || true
    if nas_is_healthy; then
        log "Network storage mounted and readable."
        return 0
    fi
    log "WARNING: Media mount is still unavailable. Retrying after cooldown."
    return 1
}

reset_wifi() {
    log "Attempting soft WiFi reset (wpa_supplicant/nmcli + interface bounce)..."
    wpa_cli -i "$INTERFACE" reconfigure 2>/dev/null || nmcli device connect "$INTERFACE" 2>/dev/null || true
    ip link set "$INTERFACE" down 2>/dev/null || true
    sleep 2
    ip link set "$INTERFACE" up 2>/dev/null || true
    systemctl restart wpa_supplicant@"$INTERFACE" 2>/dev/null || true
    nmcli device connect "$INTERFACE" 2>/dev/null || true
    # Give it time to reassociate
    sleep 15
}


# Every restart must see a readable NAS; Docker resolves the bind at creation.
restart_application() {
    if ! nas_is_healthy; then
        log "WARNING: NAS unavailable. Deferring application restart to avoid an empty media bind."
        return 1
    fi
    timeout -k 1 5 systemctl reset-failed piTrove.service 2>/dev/null || true
    timeout -k 2 55 systemctl restart piTrove.service 2>/dev/null
}

recover_online() {
    # All recovery paths share a cooldown, including failed service restarts.
    if (( LAST_RECOVERY >= 0 && SECONDS - LAST_RECOVERY < CIFS_REMOUNT_COOLDOWN )); then return; fi

    local reason health refresh=false
    if ! nas_is_healthy; then
        reason="Media mount is unavailable."
        refresh=true
    elif ! timeout -k 1 5 systemctl is-active --quiet piTrove.service 2>/dev/null; then
        reason="piTrove.service is inactive."
        refresh=true
    else
        health=$(container_health || true)
        if [[ "$health" == unhealthy ]]; then
            reason="piTrove container is unhealthy."
            refresh=true
        elif container_bind_is_stale; then
            reason="Container media bind is stale."
        elif [[ "$WAS_OFFLINE" == true && "$health" == healthy ]]; then
            log "Network recovered and piTrove can read the media mount."
            WAS_OFFLINE=false
            return
        else
            return
        fi
    fi

    LAST_RECOVERY=$SECONDS
    log "$reason Starting recovery."
    if [[ "$refresh" == true ]] && ! refresh_nas_mount; then return; fi
    if restart_application; then
        WAS_OFFLINE=false
    else
        log "Application restart deferred or failed. Retrying after cooldown."
    fi
}

main() {
    # Source optional config override (installed by install.sh)
    CONF_FILE="/etc/pitrove/wdog.conf"
    if [ -f "$CONF_FILE" ]; then
        # shellcheck source=/dev/null
        source "$CONF_FILE"
    fi

    if [[ ! "$CIFS_REMOUNT_COOLDOWN" =~ ^[1-9][0-9]{0,4}$ ]]; then
        CIFS_REMOUNT_COOLDOWN=60
    fi

    # If disabled by flag file, exit silently
    if [ -f /etc/pitrove/wdog-disable ]; then
        return 0
    fi
    # ── Main Loop ───────────────────────────────────────────────────────────────
    log "Watchdog started. Monitoring gateway $GATEWAY on $INTERFACE every 15s."

    while true; do
        if network_is_ok; then
            recover_online
            FAIL_COUNT=0
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            log "Network check failed ($FAIL_COUNT/$MAX_FAIL)"
            if [ "$FAIL_COUNT" -ge 3 ]; then
                WAS_OFFLINE=true
            fi

            if [ "$FAIL_COUNT" -ge "$MAX_FAIL" ]; then
                log "Network offline for ~3 minutes. Attempting WiFi & network service recovery..."
                FAIL_COUNT=0
                reset_wifi
                systemctl restart systemd-networkd 2>/dev/null || systemctl restart NetworkManager 2>/dev/null || true
                sleep 15
                # Never execute hard system reboot solely for network outages;
                # the app continues running in Offline Mode using local cache without reboot loops.
            fi
        fi

        sleep 15
    done

}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
