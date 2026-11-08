# am-droid OS

A lightweight, from-scratch operating system for x86_64 (AMD64) that boots on both
emulators (QEMU) and real hardware (BIOS and UEFI via GRUB).

## Features

- Custom multiboot2 bootloader stub (32->64 bit transition)
- Monolithic 64-bit kernel
- Physical memory manager (bitmap) + kernel heap
- IDT/PIC/PIT timer (100 Hz) + PS/2 keyboard
- Serial console (COM1) + VGA text mode
- Virtual filesystem (ramfs) + FAT32 reader (via ATA PIO)
- Cooperative task scheduler
- System call interface (int 0x80)
- Interactive shell
- Built-in Fuss interpreter (`fuss <file.fas>`), with floating point

## Operating system features (added in this round)

- **Processes:** ring-3 user programs in their own address spaces, preemptive
  round-robin scheduling of user processes, and system calls via `int 0x80`
  (write, exit, yield, getpid, ticks). Commands: `run <file> [&]`, `ps`, `kill`,
  `userdemo`.
- **Programs:** static ELF64 executables are loaded from the disk (`run`), and
  `install a.b.c.d port /path NAME` downloads a program over HTTP onto the disk.
- **Disk:** FAT32 read/write in the root and read-only subdirectories, with
  crash-safe overwrites; read-only ext2 on the secondary IDE drive (`ext2ls`,
  `ext2cat`).
- **Terminal:** a framebuffer text console with scrollback (PgUp/PgDn), a line
  editor with history, cursor keys and tab completion, Shift and Ctrl keys.
- **Accounts:** `USERS.DB` (SHA-256 with salt) and `PERMS.DB` (owner and mode per
  file). Commands: `login`, `whoami`, `adduser`, `passwd`, `chmod`.
- **Hardware:** RTC (`date`), PCI (`lspci`), Intel HD Audio output (`beep`),
  e1000 and virtio-net with ARP, IPv4, ICMP (`ping`), UDP, and a TCP client
  (`wget`), and an xHCI USB controller with HID boot keyboards.

Still missing: a file system on USB disks (the USB disk is reachable at block
level only), USB hubs deeper than one level and the rest of the USB device
classes, audio input, dynamic linking for ELF programs, signals, journaling for
FAT32 writes (a crash in the middle of a directory update can still lose data),
and TCP congestion control (sends are stop-and-wait).

## Build

Requirements: gcc, nasm/as, ld, grub-mkrescue, xorriso, qemu (optional).

```sh
make
```

Produces `out/amdroid.iso` and `out/amdroid.elf`.

## Run in QEMU

```sh
make run          # BIOS, VGA + serial
```

Or manually:

```sh
qemu-system-x86_64 -cdrom out/amdroid.iso -m 256M -nographic
```

With a disk image:

```sh
qemu-system-x86_64 -cdrom out/amdroid.iso -hda disk.img -boot d -m 256M -serial stdio
```

## Run on real hardware

1. Build the ISO: `make`
2. Write to USB stick (replace /dev/sdX with your USB device):
   ```sh
   sudo dd if=out/amdroid.iso of=/dev/sdX bs=4M status=progress && sync
   ```
3. Boot the machine from USB (BIOS or UEFI). VGA text console and/or serial (COM1, 115200? default 38400? Actually 38400) provide output.

## Shell commands

help, ls, cat, echo, pwd, free, uptime, clear, uname, reboot, diskls, diskcat, fuss, gui

## Fuss in the OS

`fuss NAME.FAS` runs a Fuss program stored in the FAT32 root of the disk
(8.3 names, e.g. `HELLO.FAS`). The interpreter is the same language as the
host compiler in `~/py/fuss`, with these differences:

- Tree-walking interpreter in the kernel, not a compiler; it is much slower
  (a 20M-iteration loop takes about 75 s).
- Numbers print with 15 significant digits, not shortest round-trip, so the
  last digit can differ (e.g. `1/3` prints `0.333333333333333`).
- Files are read-only: `write_file`, `delete_file`, `mkdir_p` and `write_ppm`
  report an error. `import` reads from the FAT32 root only.
- Extra builtins: `input()` (keyboard/serial), `ticks()`, `sleep_ms(ms)`,
  `files()`, `file_exists(name)`, `read_file(name)`, and framebuffer
  `fb_width() fb_height() fb_clear(c) fb_rect(x,y,w,h,c) fb_text(x,y,s,fg,bg) fb_present()`.
- Runtime errors and `exit` are handled with `__builtin_setjmp`.

Build a test disk with `mkfs.fat -F 32` and `mcopy`, then boot with
`qemu-system-x86_64 -cdrom out/amdroid.iso -hda disk.img -boot d -m 256M`.

## Layout

```
src/boot/        boot.asm, isr_stubs.asm
src/kernel/      main, idt, memory, fs, sched, shell, drivers
src/lib/         console, string
include/         headers
```
