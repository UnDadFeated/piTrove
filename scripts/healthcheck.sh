#!/usr/bin/env bash
set -euo pipefail

HB="${PITROVE_HEARTBEAT:-/app/cache/run/heartbeat}"
MAX_AGE=15

if [[ ! -f "$HB" ]]; then
  echo "missing heartbeat"
  exit 1
fi

HB_TIME=$(cat "$HB")
if [[ ! "$HB_TIME" =~ ^[0-9]{1,12}$ ]]; then
  echo "invalid heartbeat"
  exit 1
fi

read -r UPTIME _ < /proc/uptime
NOW=${UPTIME%%.*}
AGE=$((NOW - 10#$HB_TIME))
if (( AGE < 0 || AGE > MAX_AGE )); then
  echo "stale heartbeat: ${AGE}s"
  exit 1
fi

echo "healthy"
