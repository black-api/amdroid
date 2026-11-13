#!/usr/bin/env bash
# Makes a bigger copy of a FAT32 disk image (keeping every file), e.g. for videos.
#   tools/grow-disk.sh disk.img new.img 512
# The old image is left as it is. Stop QEMU first.
set -e
old="$1"; new="$2"; mb="${3:-512}"
[ -f "$old" ] || { echo "usage: $0 old.img new.img [MB]"; exit 1; }
tmp="$(mktemp -d)"
mcopy -s -n -i "$old" "::/*" "$tmp/"
rm -f "$new"
dd if=/dev/zero of="$new" bs=1M count=0 seek="$mb" status=none
mformat -i "$new" -F -c 8 -v AMDROID ::          # FAT32, 4 KB clusters
mcopy -s -n -i "$new" "$tmp/"* "::/"
rm -rf "$tmp"
echo "$new: ${mb} MB"; minfo -i "$new" | grep -E "cluster size|big size"
