#!/usr/bin/env bash
set -Eeuo pipefail

IMAGE="pitrove:latest"
OUT="sbom.spdx.json"

docker run --rm \
  -v /var/run/docker.sock:/var/run/docker.sock \
  anchore/syft:latest \
  packages "docker:${IMAGE}" \
  -o spdx-json > "$OUT"

echo "SBOM written to $OUT"
