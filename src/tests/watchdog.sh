#!/usr/bin/env bash
set -euo pipefail
source "$1"
log() { :; }
timeout() { [[ "$1" == -k ]]; shift 3; "$@"; }

restart_count=0
refresh_count=0
status=healthy
service_active=true
nas_ready=true
systemctl() {
    case "$1" in
        is-active) [[ "$service_active" == true ]] ;;
        restart) ((restart_count+=1)) ;;
    esac
    if [[ "$1" == is-active ]]; then [[ "$service_active" == true ]]; fi
}
docker() { echo "$status"; }
container_bind_is_stale() { return 1; }
nas_is_healthy() { [[ "$nas_ready" == true ]]; }
refresh_nas_mount() { ((refresh_count+=1)); [[ "$nas_ready" == true ]]; }

WAS_OFFLINE=true
recover_online
[[ "$WAS_OFFLINE" == false && "$restart_count" == 0 && "$refresh_count" == 0 ]]
status=unhealthy
recover_online
[[ "$restart_count" == 1 && "$refresh_count" == 1 ]]

# An inactive service in the same cooldown cannot trigger another remount.
service_active=false
recover_online
[[ "$restart_count" == 1 && "$refresh_count" == 1 ]]

LAST_RECOVERY=-1
nas_ready=false
recover_online
recover_online
[[ "$restart_count" == 1 && "$refresh_count" == 2 ]]
if restart_application; then
    echo "restart allowed with an unreadable media mount" >&2
    exit 1
fi
nas_ready=true
systemctl() { return 1; }
if restart_application; then
    echo "failed systemd restart reported success" >&2
    exit 1
fi

# A forced refresh is scoped to exactly the configured fstab target.
unset -f refresh_nas_mount
source "$1"
log() { :; }
timeout() { [[ "$1" == -k ]]; shift 3; "$@"; }
nas_is_healthy() { return 0; }
mountpoint() { return 0; }
umount() { [[ "$*" == "-f -- $CIFS_MOUNT" ]]; }
mount() { [[ "$*" == "-- $CIFS_MOUNT" ]]; }
refresh_nas_mount

echo "watchdog health, cooldown, and targeted recovery checks passed"
