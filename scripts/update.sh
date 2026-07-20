#!/usr/bin/env bash
set -Eeuo pipefail

PROJECT_DIR="/opt/pitrove"
CONTAINER="piTrove"
SERVICE="pitrove"
BACKUP_DIR="${PROJECT_DIR}/backups"
TS="$(date +%Y%m%d-%H%M%S)"

mkdir -p "$BACKUP_DIR"

cd "$PROJECT_DIR"

OLD_IMAGE="$(docker inspect --format='{{.Config.Image}}' "$CONTAINER")"
OLD_IMAGE_ID="$(docker inspect --format='{{.Image}}' "$CONTAINER")"

docker tag "$OLD_IMAGE_ID" "pitrove:rollback"

cp -a config "${BACKUP_DIR}/config-${TS}"

docker compose pull "$SERVICE"
docker compose up -d "$SERVICE"

for _ in {1..20}; do
  STATUS="$(docker inspect --format='{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "$CONTAINER" || true)"
  if [[ "$STATUS" == "healthy" ]]; then
    echo "Update healthy"
    exit 0
  fi
  sleep 6
done

echo "Update failed healthcheck, rolling back" >&2

docker compose stop "$SERVICE"
docker tag "pitrove:rollback" "$OLD_IMAGE"
docker compose up -d "$SERVICE"

exit 1
