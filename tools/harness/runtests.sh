#!/usr/bin/env bash
# Runs the JS engine tests in jstests/ with jsrun. Usage: ./runtests.sh [-g N]
# A test prints "FAIL: ..." for each failed check and a summary line at the end.
set -u
H="$(cd "$(dirname "$0")" && pwd)"
[ -x "$H/jsrun" ] || "$H/build_js.sh"
fail=0
for f in "$H"/jstests/*.js; do
    case "$(basename "$f")" in _*) continue;; esac          # _check.inc.js is shared code
    out="$("$H/jsrun" -m 160 "$@" "$f" 2>/dev/null)"
    if echo "$out" | grep -q '^OK$' && ! echo "$out" | grep -q 'FAIL'; then
        printf '  ok   %-16s %s\n' "$(basename "$f")" "$(echo "$out" | grep -E 'checks,' | head -1)"
    else
        fail=$((fail + 1))
        printf '  FAIL %-16s\n' "$(basename "$f")"
        echo "$out" | grep -E 'FAIL|ERROR|^  at' | sed 's/^/       /'
    fi
done
[ "$fail" -eq 0 ] && echo "all tests passed" || echo "$fail test file(s) failed"
exit $((fail > 0))
