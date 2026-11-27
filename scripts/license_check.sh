#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

ALLOWED_LICENSES=(
  "GPL-3.0"
  "GPL-3.0-only"
  "GPL-3.0-or-later"
  "LGPL-2.1"
  "LGPL-2.1-or-later"
  "LGPL-3.0"
  "LGPL-3.0-or-later"
  "MIT"
  "BSD-2-Clause"
  "BSD-3-Clause"
  "Zlib"
  "Apache-2.0"
)

errors=0

echo "Checking project license file..."
if [[ ! -s LICENSE.md ]]; then
  echo "ERROR: Missing or empty LICENSE.md" >&2
  errors=$((errors + 1))
elif ! grep -q "GNU GENERAL PUBLIC LICENSE" LICENSE.md; then
  echo "ERROR: LICENSE.md does not contain expected GPLv3 header" >&2
  errors=$((errors + 1))
else
  echo "OK: LICENSE.md verified (GPLv3)"
fi

echo "Checking Dockerfile..."
if [[ ! -s Dockerfile ]]; then
  echo "ERROR: Missing Dockerfile" >&2
  errors=$((errors + 1))
elif grep -qiE "(non-free|non-free-firmware)" Dockerfile; then
  echo "WARNING: Dockerfile references non-free APT components; verify redistribution terms." >&2
else
  echo "OK: Dockerfile verified"
fi

echo "Checking SPDX license headers in src/..."
is_allowed_license() {
  local lic="$1" allowed
  for allowed in "${ALLOWED_LICENSES[@]}"; do
    if [[ "$lic" == "$allowed" ]]; then
      return 0
    fi
  done
  return 1
}

while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  file="${match%%:*}"
  ident="$(printf '%s' "${match#*:}" | sed -nE 's/.*SPDX-License-Identifier:[[:space:]]*([A-Za-z0-9.+-]+).*/\1/p')"
  if [[ -n "$ident" ]] && ! is_allowed_license "$ident"; then
    echo "ERROR: Disallowed SPDX license '$ident' in $file" >&2
    errors=$((errors + 1))
  fi
done < <(grep -RIn "SPDX-License-Identifier:" src/ 2>/dev/null || true)

if (( errors > 0 )); then
  echo "License check failed with $errors error(s)." >&2
  exit 1
fi

echo "License verification passed."
