#!/usr/bin/env bash
set -euo pipefail
source "$1"
log() { :; }
# Run shell doubles through the same timeout interface as production commands.
timeout() { [[ "$1" == -k && "$2" == 1 && "$3" == 5 ]]; shift 3; "$@"; }

# A substring match used to classify "unhealthy" as healthy.
docker() { echo unhealthy; }
if container_is_healthy; then
    echo "unhealthy container accepted as healthy" >&2
    exit 1
fi
docker() { echo healthy; }
container_is_healthy

restart_count=0
systemctl() { if [[ "$1" == restart ]]; then ((restart_count+=1)); fi; }
nas_is_healthy() { return 1; }
if restart_application; then
    echo "restart was allowed with unreadable NAS" >&2
    exit 1
fi
[[ "$restart_count" == 0 ]]
nas_is_healthy() { return 0; }
restart_application
[[ "$restart_count" == 1 ]]
systemctl() { return 1; }
if restart_application; then
    echo "failed restart reported success" >&2
    exit 1
fi
echo "watchdog health and NAS restart regressions passed"
