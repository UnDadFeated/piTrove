#!/usr/bin/env bash
set -Eeuo pipefail

PID="$(pgrep -f '/app/piTrove' | head -n1)"
if [[ -z "$PID" ]]; then
  echo "Error: piTrove process not found. Ensure container is running." >&2
  exit 1
fi

echo "Recording perf for 30 seconds..."
perf record -F 99 -g -p "$PID" -- sleep 30

echo "Throttling state:"
vcgencmd get_throttled || true

echo "Temperature:"
vcgencmd measure_temp || true

echo "Clock speeds:"
vcgencmd measure_clock arm || true
vcgencmd measure_clock core || true

perf report --stdio
