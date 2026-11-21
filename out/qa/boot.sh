#!/bin/sh
# boots the ISO headless with a monitor socket; usage: boot.sh [extra qemu args]
cd "$(dirname "$0")/../.."
kill $(pgrep -x qemu-system-x86) 2>/dev/null; sleep 1
rm -f out/qa/mon.sock out/qa/serial.log
qemu-system-x86_64 -accel kvm -cpu host -display none -cdrom out/amdroid.iso -serial file:out/qa/serial.log -m 1G -monitor unix:out/qa/mon.sock,server,nowait -netdev user,id=n0 -device e1000,netdev=n0 "$@" -daemonize
sleep 7
