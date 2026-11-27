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
LOG_FILE="${LOG_FILE:-/home/pi/piTrove/logs/pitrove-watchdog.log}"

FAIL_COUNT=0
MAX_FAIL=12          # 12 checks * 15 seconds = 180s (3 minutes) offline trigger
WAS_OFFLINE=false
LAST_RECOVERY=-1
NAS_FAILURES=0
RECOVERY_FAILURES=0
NEXT_BIND_PROBE=0


log() {
    if command -v logger >/dev/null 2>&1; then
        logger -t "pitrove-watchdog" "$@"
    fi

    # Also write to local log file in logs/ folder if directory exists
    LOG_FILE="${LOG_FILE:-/home/pi/piTrove/logs/pitrove-watchdog.log}"
    LOG_DIR=${LOG_FILE%/*}
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

# NAS mount is usable: attached AND readable within 10s (CIFS session alive).
# A dead CIFS session can leave the mount attached while reads hang (hard
# mount), so a plain mount check is not enough — the timed read catches it.
nas_is_healthy() {
    timeout -k 1 5 mountpoint -q "$CIFS_MOUNT" 2>/dev/null || return 1
    timeout -k 1 10 ls -U "$CIFS_MOUNT" >/dev/null 2>&1
}

container_health() {
    timeout -k 1 5 docker inspect --format='{{.State.Health.Status}}' "$DOCKER_CONTAINER" 2>/dev/null
}

# The caller has already checked that the host NAS is readable.
container_bind_is_stale() {
    local host_item result=0
    host_item=$(timeout -k 1 5 find "$CIFS_MOUNT" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) || return 1
    [[ -n "$host_item" ]] || return 1
    timeout -k 1 5 docker exec "$DOCKER_CONTAINER" /bin/sh -c '
        item=$(find /app/media -mindepth 1 -maxdepth 1 -print -quit) || exit 2
        test -n "$item" || exit 3
    ' 2>/dev/null || result=$?
    [[ "$result" == 3 ]]
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

recover_interface() {
    if [ ! -d "/sys/class/net/$INTERFACE" ]; then
        log "Recovery deferred: interface $INTERFACE is unavailable."
        return
    fi
    local supplicant="wpa_supplicant@$INTERFACE.service"
    if systemctl is-active --quiet "$supplicant" || systemctl is-enabled --quiet "$supplicant"; then
        log "Restarting the standalone supplicant for $INTERFACE."
        timeout -k 2 12 systemctl restart "$supplicant" 2>/dev/null || true
    elif systemctl is-active --quiet NetworkManager; then
        local state
        state=$(LC_ALL=C timeout -k 1 5 nmcli -g GENERAL.STATE device show "$INTERFACE" 2>/dev/null) || state=
        state=${state%% *}
        if [[ "$state" =~ ^[0-9]+$ ]] && (( state >= 20 )); then
            log "Asking NetworkManager to reconnect $INTERFACE."
            timeout -k 1 8 nmcli --wait 6 device disconnect "$INTERFACE" 2>/dev/null || true
            timeout -k 1 12 nmcli --wait 10 device connect "$INTERFACE" 2>/dev/null || true
            return
        fi
    fi
    if systemctl is-active --quiet systemd-networkd; then
        timeout -k 1 8 networkctl renew "$INTERFACE" 2>/dev/null || true
    fi
}

# Every restart must see a readable NAS; Docker resolves the bind at creation.
restart_application() {
    if ! nas_is_healthy; then
        log "WARNING: NAS unavailable. Deferring application restart to avoid an empty media bind."
        return 1
    fi
    timeout -k 1 5 systemctl reset-failed piTrove.service 2>/dev/null || true
    timeout -k 2 90 systemctl restart piTrove.service 2>/dev/null
}

recover_online() {
    local delay=$((CIFS_REMOUNT_COOLDOWN << RECOVERY_FAILURES))
    if (( LAST_RECOVERY >= 0 && SECONDS - LAST_RECOVERY < delay )); then return; fi

    local reason="" health refresh=false
    if ! nas_is_healthy; then
        NAS_FAILURES=$((NAS_FAILURES + 1))
        if (( NAS_FAILURES < 3 )); then return; fi
        reason="Media mount is unavailable."
        refresh=true
    else
        NAS_FAILURES=0
        health=$(container_health || true)
        if [[ "$health" == unhealthy ]] ||
           { [[ -z "$health" ]] && ! timeout -k 1 5 systemctl is-active --quiet piTrove.service 2>/dev/null; }; then
            reason="piTrove is inactive or unhealthy."
        elif [[ "$health" == healthy ]]; then
            if [[ "$WAS_OFFLINE" == true ]] || (( SECONDS >= NEXT_BIND_PROBE )); then
                NEXT_BIND_PROBE=$((SECONDS + 300))
                if container_bind_is_stale; then
                    reason="Container media bind is stale."
                fi
            fi
            if [[ -z "${reason:-}" ]]; then
                if [[ "$WAS_OFFLINE" == true ]]; then
                    log "Network recovered and piTrove can read the media mount."
                    WAS_OFFLINE=false
                fi
                RECOVERY_FAILURES=0
                LAST_RECOVERY=-1
                return
            fi
        else
            return
        fi
    fi

    log "$reason Starting recovery."
    if { [[ "$refresh" == false ]] || refresh_nas_mount; } && restart_application; then
        WAS_OFFLINE=false
        NAS_FAILURES=0
        RECOVERY_FAILURES=0
        NEXT_BIND_PROBE=$((SECONDS + 300))
    else
        log "Application restart deferred or failed. Retrying after cooldown."
        if (( RECOVERY_FAILURES < 4 )); then RECOVERY_FAILURES=$((RECOVERY_FAILURES + 1)); fi
    fi
    LAST_RECOVERY=$SECONDS
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
                recover_interface
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
