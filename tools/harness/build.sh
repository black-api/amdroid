#!/usr/bin/env bash
# Build the Am-Droid web engine as a host renderer.
set -e
H="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$H/../.." && pwd)"
CC=${CC:-gcc}
CFLAGS="-O2 -g -w -DWEB_HOST_DEBUG -fno-strict-aliasing -I$H/shim -I$ROOT/include"

SRCS=(
  "$ROOT/src/kernel/web/html.c"
  "$ROOT/src/kernel/web/css.c"
  "$ROOT/src/kernel/web/layout.c"
  "$ROOT/src/kernel/web/jsdom.c"
  "$ROOT/src/kernel/web/image.c"
  "$ROOT/src/kernel/web/svg.c"
  "$ROOT/src/kernel/web/webp.c"
  "$ROOT/src/kernel/js/js.c"
  "$ROOT/src/kernel/js/regex.c"
  "$ROOT/src/user/husser/painter.c"
  "$H/host_heap.c"
  "$ROOT/src/lib/string.c"
  "$ROOT/src/lib/klib.c"
  "$ROOT/src/lib/kmath.c"
  "$ROOT/src/lib/markdown.c"
  "$H/host_main.c"
)
mkdir -p "$H/obj"
OBJS=()
for s in "${SRCS[@]}"; do
  o="$H/obj/$(basename "${s%.c}").o"
  $CC $CFLAGS -c "$s" -o "$o"
  OBJS+=("$o")
done
$CC "${OBJS[@]}" -o "$H/render" -lm
echo "built: $H/render"

# Render + convert a page if given: build.sh render page.html [w] [h]
if [ "$1" = "render" ]; then
  shift
  page="$1"; w="${2:-1200}"; hh="${3:-8000}"
  "$H/render" "$page" "$H/out.ppm" "$w" "$hh"
  pnmtopng "$H/out.ppm" > "$H/out.png"
  echo "image: $H/out.png"
fi
