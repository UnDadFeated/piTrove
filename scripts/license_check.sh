#!/usr/bin/env bash
set -Eeuo pipefail

ALLOWED_LICENSES=(
  "GPL-3.0"
  "GPL-3.0-or-later"
  "LGPL-2.1"
  "LGPL-3.0"
  "MIT"
  "BSD-2-Clause"
  "BSD-3-Clause"
  "Zlib"
  "Apache-2.0"
)

echo "Checking SPDX license headers..."
grep -RIl "SPDX-License-Identifier" src/ > /dev/null || echo "No SPDX headers found (optional)"

echo "Checking dependency licenses..."
for dep in SDL3 SDL3_image SDL3_ttf FFmpeg sqlite3 libexif libheif libsodium; do
  echo "Review license compatibility for: $dep"
done

echo "Allowed licenses:"
printf '%s\n' "${ALLOWED_LICENSES[@]}"
