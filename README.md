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

help, ls, cat, echo, pwd, free, uptime, clear, uname, reboot, diskls, diskcat

## Layout

```
src/boot/        boot.asm, isr_stubs.asm
src/kernel/      main, idt, memory, fs, sched, shell, drivers
src/lib/         console, string
include/         headers
```
