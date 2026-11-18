#!/usr/bin/env bash
set -euo pipefail
update="$1"
healthcheck="$2"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work"/{bin,project/config}
printf 'test-value\n' > "$work/project/config/config.toml"
export PITROVE_DIR="$work/project"
export PITROVE_TEST_WORK="$work"
export PATH="$work/bin:$PATH"

cat > "$work/bin/git" <<'GIT'
#!/usr/bin/env bash
if [[ "$*" == 'branch --show-current' ]]; then
    echo develop
elif [[ "$*" == 'pull --ff-only origin develop' ]]; then
    [[ "$CASE" != pull-failure ]]
else
    exit 2
fi
GIT
cat > "$work/bin/docker" <<'DOCKER'
#!/usr/bin/env bash
set -eu
printf '%s\n' "$*" >> "$PITROVE_TEST_WORK/docker"
case "$*" in
    'compose config --images pitrove') echo pitrove-unit ;;
    'inspect --format={{.Image}} piTrove') echo sha256:old ;;
    'image tag sha256:old pitrove:rollback'|'compose build pitrove') ;;
    'image inspect --format={{.Id}} pitrove-unit') echo sha256:new ;;
    'compose up -d pitrove') [[ "$CASE" != start-failure ]] ;;
    'image tag sha256:old pitrove-unit') touch "$PITROVE_TEST_WORK/rolled-back" ;;
    'compose up -d --force-recreate pitrove') [[ -f "$PITROVE_TEST_WORK/rolled-back" ]] ;;
    'inspect --format={{.Image}} {{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}} piTrove')
        if [[ "$CASE" == success ]]; then
            echo 'sha256:new healthy'
        else
            echo 'sha256:old healthy'
        fi ;;
    *) exit 2 ;;
esac
DOCKER
printf '#!/usr/bin/env bash\nexit 0\n' > "$work/bin/sleep"
chmod +x "$work"/bin/*
for scenario in success pull-failure start-failure stale-container; do
    export CASE="$scenario"
    : > "$work/docker"
    rm -f "$work/rolled-back"
    if bash "$update" > "$work/update.log" 2>&1; then
        [[ "$scenario" == success ]]
        grep -qx 'Update healthy' "$work/update.log"
    else
        [[ "$scenario" != success ]]
        if [[ "$scenario" == pull-failure ]]; then
            [[ ! -s "$work/docker" ]]
        else
            grep -qx 'compose up -d --force-recreate pitrove' "$work/docker"
            grep -qx 'Previous image healthy' "$work/update.log"
        fi
    fi
done
[[ "$(stat -c '%a' "$work/project/backups")" == 700 ]]

export PITROVE_HEARTBEAT="$work/heartbeat"
date +%s > "$PITROVE_HEARTBEAT"
bash "$healthcheck" | grep -qx healthy
for value in '' 'invalid' "$(( $(date +%s) + 7200 ))" "$(( $(date +%s) - 7200 ))"; do
    printf '%s\n' "$value" > "$PITROVE_HEARTBEAT"
    if bash "$healthcheck" >/dev/null; then
        echo "invalid or stale heartbeat reported healthy" >&2
        exit 1
    fi
done
echo "update rollback, image identity, and heartbeat checks passed"
