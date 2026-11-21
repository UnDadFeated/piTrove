#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

PROJECT_DIR="${PITROVE_DIR:-/home/pi/piTrove}"
if [[ ! -d "$PROJECT_DIR" ]]; then
  PROJECT_DIR="/opt/pitrove"
fi
CONTAINER="piTrove"
SERVICE="pitrove"
UNIT="piTrove.service"
BACKUP_DIR="${PROJECT_DIR}/backups"
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
cd "$PROJECT_DIR"

if [[ "${1:-}" == --cron ]]; then
  mode="$(python3 -B - "$SCRIPT_DIR" <<'CONFIG'
from pathlib import Path
import re
import sys

sys.path.insert(0, sys.argv[1])
from merge_config import SETTING, split_sections

try:
    config = Path("config/config.toml").read_text(encoding="utf-8")
except FileNotFoundError:
    config = ""
updates = split_sections(config).get("updates", ())
enabled = False
for line in updates:
    key = SETTING.match(line)
    if key and key[1] == "auto_update":
        value = line.split("=", 1)[1].strip()
        enabled = re.fullmatch(r"""(?:(["'])(1|true)\1|(1|true))\s*(?:[#;].*)?""", value) is not None
print("enabled" if enabled else "disabled")
CONFIG
)" || exit 1
  [[ "$mode" == enabled ]] || exit 0
fi

if [[ "$(git branch --show-current)" != "develop" ]]; then
  echo "Update requires the develop branch" >&2
  exit 1
fi
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "Commit or set aside local changes before updating" >&2
  exit 1
fi

install -d -m 700 "$BACKUP_DIR"
exec 9>"$BACKUP_DIR/.update.lock"
if ! flock -n 9; then
  echo "Another piTrove update is running" >&2
  exit 1
fi

previous_revision="$(git rev-parse HEAD)"
previous_image=
previous_image_name=

managed=false
if systemctl is-active --quiet "$UNIT" 2>/dev/null; then managed=true; fi
service_touched=false
config_backup=
config_ready=false

stop_service() {
  if [[ "$managed" == true ]]; then
    sudo -n systemctl stop "$UNIT"
  else
    docker compose stop "$SERVICE"
  fi
}

start_service() {
  if [[ "$managed" == true ]]; then
    sudo -n systemctl start "$UNIT"
  else
    docker compose up -d --force-recreate "$SERVICE"
  fi
}

wait_for_health() {
  local expected="$1" state
  for _ in {1..24}; do
    state="$(docker inspect --format='{{.Image}} {{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "$CONTAINER" 2>/dev/null || true)"
    if [[ "$state" == "$expected healthy" ]]; then return 0; fi
    if [[ "$state" == *' unhealthy' ]]; then return 1; fi
    sleep 5
  done
  return 1
}

rollback() {
  local prior_status=$?
  if (( prior_status == 0 )); then return; fi
  trap - EXIT
  set +e
  echo "Update failed. Restoring the previous revision." >&2
  if [[ "$service_touched" == true ]] && ! stop_service; then
    echo "Could not stop the service for a safe rollback" >&2
    exit "$prior_status"
  fi
  git reset --keep "$previous_revision" || echo "Could not restore the previous Git revision" >&2
  if [[ "$config_ready" == true ]]; then
    for name in config.toml secrets.toml; do
      if [[ -f "$config_backup/$name" ]]; then
        cp -p -- "$config_backup/$name" "config/$name"
      else
        rm -f -- "config/$name"
      fi
    done
  fi
  if [[ -n "$previous_image" ]]; then
    if docker image tag "$previous_image" "$previous_image_name" &&
       { [[ "$service_touched" == false ]] || { start_service && wait_for_health "$previous_image"; }; }; then
      echo "Previous image restored" >&2
    else
      echo "Previous image could not be restored" >&2
    fi
  fi
  exit "$prior_status"
}
trap rollback EXIT

git pull --ff-only origin develop
if [[ "${1:-}" == --cron && "$(git rev-parse HEAD)" == "$previous_revision" ]]; then
  trap - EXIT
  echo "piTrove is up to date"
  exit 0
fi
previous_image="$(docker inspect --format='{{.Image}}' "$CONTAINER" 2>/dev/null || true)"
if [[ -n "$previous_image" ]]; then
  previous_image_name="$(docker inspect --format='{{.Config.Image}}' "$CONTAINER")"
  if [[ -z "$previous_image_name" || "$previous_image_name" == *$'\n'* ]]; then
    echo "Could not resolve the previous piTrove image" >&2
    exit 1
  fi
  docker image tag "$previous_image" pitrove:rollback
fi
new_image_name="$(docker compose config --images "$SERVICE")"
if [[ -z "$new_image_name" || "$new_image_name" == *$'\n'* ]]; then exit 1; fi
env BUILDKIT_PROGRESS=plain docker compose build "$SERVICE"
new_image="$(docker image inspect --format='{{.Id}}' "$new_image_name")"

media_dir="$(docker inspect --format='{{range .Mounts}}{{if eq .Destination "/app/media"}}{{.Source}}{{end}}{{end}}' "$CONTAINER" 2>/dev/null || true)"
if [[ "$media_dir" == /mnt/nas ]]; then mountpoint -q "$media_dir"; fi
if [[ -n "$media_dir" ]]; then timeout -k 2 8 ls -U -- "$media_dir" >/dev/null; fi

service_touched=true
stop_service
if [[ -d config ]]; then
  config_backup="$(mktemp -d "$BACKUP_DIR/config-$(date +%Y%m%d-%H%M%S).XXXXXX")"
  for name in config.toml secrets.toml; do
    if [[ -f "config/$name" ]]; then cp -p -- "config/$name" "$config_backup/$name"; fi
  done
  config_ready=true
fi

start_service
wait_for_health "$new_image"
trap - EXIT
echo "Update healthy"
