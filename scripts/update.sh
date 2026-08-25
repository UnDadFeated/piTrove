#!/usr/bin/env bash
set -Eeuo pipefail

PROJECT_DIR="${PITROVE_DIR:-/home/pi/piTrove}"
if [[ ! -d "$PROJECT_DIR" ]]; then
  PROJECT_DIR="/opt/pitrove"
fi
CONTAINER="piTrove"
SERVICE="pitrove"
BACKUP_DIR="${PROJECT_DIR}/backups"
TS="$(date +%Y%m%d-%H%M%S)"

mkdir -p "$BACKUP_DIR"

cd "$PROJECT_DIR"

OLD_IMAGE="$(docker inspect --format='{{.Config.Image}}' "$CONTAINER" 2>/dev/null || echo "pitrove-pitrove:latest")"
OLD_IMAGE_ID="$(docker inspect --format='{{.Image}}' "$CONTAINER" 2>/dev/null || echo "")"

if [[ -n "$OLD_IMAGE_ID" ]]; then
  docker tag "$OLD_IMAGE_ID" "pitrove:rollback" 2>/dev/null || true
fi

if [[ -d "config" ]]; then
  cp -a config "${BACKUP_DIR}/config-${TS}"
fi

git pull origin develop 2>/dev/null || git pull origin main 2>/dev/null || true
docker compose build "$SERVICE"
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
