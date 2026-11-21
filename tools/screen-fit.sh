#!/bin/sh
# screen-fit.sh — the guest's screen size, printed as WIDTHxHEIGHTx32.
#
# The QEMU window is as big as the guest's screen, so the guest asks for a mode that leaves
# the window room on the host's display (about 85% of its height and width) instead of
# covering the whole of it. Only modes GRUB and real graphics cards know are offered, and no
# more than the 1080p the GUI is built for. When the host's screen cannot be read, the full-HD
# default is printed. make GFXMODE=... overrides all of this.
set -u

sw=0
sh=0
dims=""

if [ -n "${DISPLAY:-}" ] && command -v xdpyinfo >/dev/null 2>&1; then
    dims=$(xdpyinfo 2>/dev/null | awk '/dimensions:/ { print $2; exit }')
fi
if [ -z "$dims" ] && [ -n "${DISPLAY:-}" ] && command -v xrandr >/dev/null 2>&1; then
    dims=$(xrandr --current 2>/dev/null | awk '
        / connected/ {
            for(i = 1; i <= NF; i++)
                if($i ~ /^[0-9]+x[0-9]+[+]/) { match($i, /^[0-9]+x[0-9]+/); print substr($i, RSTART, RLENGTH); exit }
        }')
fi

case "$dims" in
    *x*) sw=${dims%x*}; sh=${dims#*x} ;;
esac
case "$sw" in ''|*[!0-9]*) sw=0 ;; esac
case "$sh" in ''|*[!0-9]*) sh=0 ;; esac

# The largest common mode whose window (plus its frame and the taskbar) still fits.
for mode in 1920x1080 1600x900 1280x720 1024x768 800x600; do
    w=${mode%x*}
    h=${mode#*x}
    if [ "$sw" -le 0 ] || [ "$sh" -le 0 ] ||
       { [ $((w + 64)) -le "$sw" ] && [ $((h + 72)) -le "$sh" ]; }; then
        echo "${mode}x32"
        exit 0
    fi
done
echo 800x600x32
