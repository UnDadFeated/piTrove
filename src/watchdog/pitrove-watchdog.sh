#!/usr/bin/env bash
# piTrove Network Watchdog - External system-level monitor
# Installed by install.sh to /usr/local/bin/pitrove-watchdog.sh
# Runs as a systemd service independent of Docker.
# Checks network health every 15s, attempts WiFi reset, then clean reboot if needed.
set -euo pipefail

GATEWAY="${GATEWAY:-192.168.4.1}"
INTERFACE="${INTERFACE:-wlan0}"
FAIL_COUNT=0
MAX_FAIL=2          # 2 checks * 15 seconds = 30s offline trigger
WIFI_RESET_DONE=false

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
    logger -t "pitrove-watchdog" "$@"
}

network_is_ok() {
    # Check 1: Does wlan0 exist and have carrier?
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
    # Use clean reboot (systemd) — properly shuts down WiFi firmware vs dirty SysRq
    reboot
}

# ── Main Loop ───────────────────────────────────────────────────────────────
log "Watchdog started. Monitoring gateway $GATEWAY on $INTERFACE every 15s."

while true; do
    if network_is_ok; then
        FAIL_COUNT=0
        WIFI_RESET_DONE=false
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        log "Network check failed ($FAIL_COUNT/$MAX_FAIL)"

        if [ "$FAIL_COUNT" -ge "$MAX_FAIL" ]; then
            if [ "$WIFI_RESET_DONE" = false ]; then
                log "Network down for 30s. Attempting one WiFi reset before reboot..."
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
