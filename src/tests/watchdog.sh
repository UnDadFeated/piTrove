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
}
docker() { echo "$status"; }
container_bind_is_stale() { return 1; }
nas_is_healthy() { [[ "$nas_ready" == true ]]; }
refresh_nas_mount() { ((refresh_count+=1)); [[ "$nas_ready" == true ]]; }

WAS_OFFLINE=true
RECOVERY_FAILURES=3
recover_online
[[ "$WAS_OFFLINE" == false && "$restart_count" == 0 && "$refresh_count" == 0 && "$RECOVERY_FAILURES" == 0 ]]
status=unhealthy
recover_online
[[ "$restart_count" == 1 && "$refresh_count" == 0 ]]

# An inactive service in the same cooldown cannot trigger another remount.
service_active=false
recover_online
[[ "$restart_count" == 1 && "$refresh_count" == 0 ]]

LAST_RECOVERY=-1
nas_ready=false
recover_online
recover_online
[[ "$restart_count" == 1 && "$refresh_count" == 0 ]]
recover_online
[[ "$restart_count" == 1 && "$refresh_count" == 1 && "$RECOVERY_FAILURES" == 1 ]]
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
unset -f refresh_nas_mount container_bind_is_stale
source "$1"
log() { :; }
timeout() { [[ "$1" == -k ]]; shift 3; "$@"; }
nas_is_healthy() { return 0; }
mountpoint() { return 0; }
umount() { [[ "$*" == "-f -- $CIFS_MOUNT" ]]; }
mount() { [[ "$*" == "-- $CIFS_MOUNT" ]]; }
refresh_nas_mount

find() { echo fixture.jpg; }
exec_status=0
docker() { [[ "$1" == exec ]] && return "$exec_status"; }
for exec_status in 0 1 2 124 137; do
    if container_bind_is_stale; then
        echo "exit $exec_status incorrectly marked the media bind stale" >&2
        exit 1
    fi
done
exec_status=3
container_bind_is_stale

echo "watchdog debounce, cooldown, targeted recovery, and stale-bind checks passed"
