#!/usr/bin/env bash
set -euo pipefail

HB="/app/cache/run/heartbeat"
MAX_AGE=15

if [[ ! -f "$HB" ]]; then
  echo "missing heartbeat"
  exit 1
fi

NOW=$(date +%s)
HB_TIME=$(cat "$HB")
AGE=$((NOW - HB_TIME))

if (( AGE > MAX_AGE )); then
  echo "stale heartbeat: ${AGE}s"
  exit 1
fi

echo "healthy"
