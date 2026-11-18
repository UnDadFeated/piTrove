#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

PROJECT_DIR="${PITROVE_DIR:-/home/pi/piTrove}"
if [[ ! -d "$PROJECT_DIR" ]]; then
  PROJECT_DIR="/opt/pitrove"
fi
CONTAINER="piTrove"
SERVICE="pitrove"
BACKUP_DIR="${PROJECT_DIR}/backups"

cd "$PROJECT_DIR"

if [[ "$(git branch --show-current)" != "develop" ]]; then
  echo "Update requires the develop branch" >&2
  exit 1
fi

install -d -m 700 "$BACKUP_DIR"
exec 9>"$BACKUP_DIR/.update.lock"
if ! flock -n 9; then
  echo "Another piTrove update is running" >&2
  exit 1
fi

git pull --ff-only origin develop

if [[ -d config ]]; then
  backup="$(mktemp -d "$BACKUP_DIR/config-$(date +%Y%m%d-%H%M%S).XXXXXX")"
  cp -a config/. "$backup/"
fi

service_image="$(docker compose config --images "$SERVICE")"
if [[ -z "$service_image" || "$service_image" == *$'\n'* ]]; then
  echo "Could not resolve the piTrove service image" >&2
  exit 1
fi
old_image_id="$(docker inspect --format='{{.Image}}' "$CONTAINER" 2>/dev/null || true)"
if [[ -n "$old_image_id" ]]; then
  docker image tag "$old_image_id" pitrove:rollback
fi

env BUILDKIT_PROGRESS=plain docker compose build "$SERVICE"
new_image_id="$(docker image inspect --format='{{.Id}}' "$service_image")"

wait_for_health() {
  local expected="$1" state
  for _ in {1..20}; do
    state="$(docker inspect --format='{{.Image}} {{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "$CONTAINER" 2>/dev/null || true)"
    if [[ "$state" == "$expected healthy" ]]; then return 0; fi
    sleep 6
  done
  return 1
}

if docker compose up -d "$SERVICE" && wait_for_health "$new_image_id"; then
  echo "Update healthy"
  exit 0
fi

echo "Update failed to start or become healthy" >&2
if [[ -n "$old_image_id" ]]; then
  echo "Restoring previous piTrove image" >&2
  if docker image tag "$old_image_id" "$service_image" &&
     docker compose up -d --force-recreate "$SERVICE" &&
     wait_for_health "$old_image_id"; then
    echo "Previous image healthy" >&2
  else
    echo "Rollback did not become healthy" >&2
  fi
fi
exit 1
