#!/usr/bin/env bash
set -Eeuo pipefail

PID="$(pgrep -x piTrove | head -n1 || true)"
if [[ -z "$PID" ]]; then
  echo "Error: piTrove process not found. Ensure container is running." >&2
  exit 1
fi

echo "Recording perf for 30 seconds..."
perf record -F 99 -g -p "$PID" -- sleep 30

echo "Throttling state:"
if command -v vcgencmd >/dev/null 2>&1; then
  vcgencmd get_throttled 2>/dev/null || echo "vcgencmd get_throttled unavailable"
else
  echo "vcgencmd unavailable"
fi

echo "Temperature:"
if command -v vcgencmd >/dev/null 2>&1 && vcgencmd measure_temp 2>/dev/null; then
  :
elif [[ -r /sys/class/thermal/thermal_zone0/temp ]]; then
  awk '{printf "temp=%.1f\x27C\n", $1 / 1000.0}' /sys/class/thermal/thermal_zone0/temp
else
  echo "Temperature sensor unavailable"
fi

echo "Clock speeds:"
if command -v vcgencmd >/dev/null 2>&1; then
  vcgencmd measure_clock arm 2>/dev/null || echo "arm clock unavailable"
  vcgencmd measure_clock core 2>/dev/null || echo "core clock unavailable"
else
  echo "vcgencmd unavailable"
fi

perf report --stdio
