#!/usr/bin/env bash
set -euo pipefail

launcher="${1:?launcher path required}"
scratch="$(mktemp -d /tmp/dart-launcher-restart.XXXXXX)"
trap 'rm -rf "$scratch"' EXIT
printf '%s %s %s &!\n' "$(dirname "$0")/launcher_child_exit.sh" \
    "$scratch/startup.list" "$scratch/inherited-startup" > "$scratch/startup.list"
timeout 8 "$launcher" --poll-ms 100 --heartbeat 0 --log-dir "$scratch/logs" "$scratch/startup.list" || code=$?
[[ "${code:-0}" == 124 ]] || { echo "launcher exited unexpectedly" >&2; exit 1; }
starts="$(rg -c 'op=start .*mode=bg app=launcher_child_exit.sh' "$scratch/logs/launcher.log")"
[[ "$starts" -ge 2 ]] || { echo "expected a restart, got $starts starts" >&2; exit 1; }
rg -q 'result=done_marker' "$scratch/logs/launcher.log" || {
    echo "expected the completion marker to trigger a restart" >&2
    exit 1
}
[[ ! -e "$scratch/inherited-startup" ]] || {
    echo "the application inherited the open startup list" >&2
    exit 1
}
printf 'launcher restart passed: %s starts\n' "$starts"

# Foreground jobs must release the parent's startup reader too. A staged
# replacement is activated once, and the prior configuration is preserved.
printf '/bin/false\n' > "$scratch/foreground.list"
printf '%s %s %s\n' "$(dirname "$0")/launcher_child_exit.sh" \
    "$scratch/foreground.list" "$scratch/inherited-foreground" > "$scratch/foreground.list.next"
"$launcher" -t --log-dir "$scratch/dry-logs" "$scratch/foreground.list" >/dev/null
[[ -f "$scratch/foreground.list.next" ]] || { echo 'dry run activated update' >&2; exit 1; }
timeout 5 "$launcher" -n --poll-ms 100 --heartbeat 0 --log-dir "$scratch/fg-logs" "$scratch/foreground.list"
[[ ! -f "$scratch/foreground.list.next" ]] || { echo 'update not activated' >&2; exit 1; }
[[ "$(cat "$scratch/foreground.list.previous")" == /bin/false ]] || { echo 'old list not preserved' >&2; exit 1; }
[[ ! -e "$scratch/inherited-foreground" ]] || { echo 'foreground startup reader remains open' >&2; exit 1; }
rg -q 'op=list_update result=applied' "$scratch/fg-logs/launcher.log"
printf 'launcher foreground snapshot and staged update passed\n'
