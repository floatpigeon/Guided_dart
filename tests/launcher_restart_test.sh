#!/usr/bin/env bash
set -euo pipefail

launcher="${1:?launcher path required}"
scratch="$(mktemp -d /tmp/dart-launcher-restart.XXXXXX)"
trap 'rm -rf "$scratch"' EXIT
printf '%s &!\n' "$(dirname "$0")/launcher_child_exit.sh" > "$scratch/startup.list"
timeout 8 "$launcher" --poll-ms 100 --heartbeat 0 --log-dir "$scratch/logs" "$scratch/startup.list" || code=$?
[[ "${code:-0}" == 124 ]] || { echo "launcher exited unexpectedly" >&2; exit 1; }
starts="$(rg -c 'op=start .*mode=bg app=launcher_child_exit.sh' "$scratch/logs/launcher.log")"
[[ "$starts" -ge 2 ]] || { echo "expected a restart, got $starts starts" >&2; exit 1; }
rg -q 'result=done_marker' "$scratch/logs/launcher.log" || {
    echo "expected the completion marker to trigger a restart" >&2
    exit 1
}
printf 'launcher restart passed: %s starts\n' "$starts"
