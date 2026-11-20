#!/usr/bin/env bash
# Build jsrun: Am-Droid's JS engine on the host. Usage: ./build_js.sh  then  ./jsrun file.js
set -e
H="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$H/../.." && pwd)"
gcc -O2 -g -w -fno-strict-aliasing -I"$H/shim" -I"$ROOT/include" -I"$ROOT/src/lib" \
  "$ROOT/src/kernel/js/js.c" "$ROOT/src/kernel/js/regex.c" "$H/host_heap.c" \
  "$ROOT/src/lib/string.c" "$ROOT/src/lib/klib.c" "$ROOT/src/lib/kmath.c" "$H/jsrun.c" -o "$H/jsrun" -lm
echo "built: $H/jsrun"
