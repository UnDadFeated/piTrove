#!/usr/bin/env bash
set -Eeuo pipefail

IMAGE="${1:-pitrove-pitrove:latest}"
OUT="${2:-sbom.spdx.json}"
SYFT_IMAGE="${SYFT_IMAGE:-anchore/syft:v1.26.1}"

docker run --rm \
  -v /var/run/docker.sock:/var/run/docker.sock \
  "$SYFT_IMAGE" \
  packages "docker:${IMAGE}" \
  -o spdx-json > "$OUT"

echo "SBOM written to $OUT"
