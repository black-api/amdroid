#!/usr/bin/env bash
# Converts any video into an MVP (Mad Vid Player) file and puts it on the OS's disk.
#   tools/mvp-convert.sh input.mp4 NAME [disk.img]
# NAME is an 8.3 name without the extension (e.g. MYVID): it becomes VIDEOS/NAME.AVI.
# MVP plays Motion-JPEG video + 16-bit PCM audio in AVI; the OS decodes every frame itself.
# Default: 1280x720 at 15 fps (what the OS decodes in real time under QEMU without KVM).
# Other sizes: MVP_SIZE=640:360 MVP_FPS=30 tools/mvp-convert.sh ...
# Stop QEMU first: the disk must not be in use.
set -e
in="$1"; name="${2:-VIDEO}"; disk="${3:-disk.img}"
[ -f "$in" ] || { echo "usage: $0 input.mp4 NAME [disk.img]"; exit 1; }
out="$(mktemp -d)/${name^^}.AVI"
ffmpeg -loglevel error -y -i "$in" -vf "scale=${MVP_SIZE:-1280:720},fps=${MVP_FPS:-15}" -c:v mjpeg -q:v 8 -pix_fmt yuvj420p \
       -c:a pcm_s16le -ar 24000 -ac 1 "$out"
mmd -i "$disk" ::/VIDEOS 2>/dev/null || true
mcopy -o -i "$disk" "$out" "::/VIDEOS/${name^^}.AVI"
echo "added VIDEOS/${name^^}.AVI ($(stat -c %s "$out") bytes) to $disk"
