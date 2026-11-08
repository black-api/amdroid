CC = gcc
AS = as
LD = ld
CFLAGS = -std=gnu11 -ffreestanding -O0 -Wall -Wextra -fno-stack-protector -fno-pie -mno-red-zone -m64 -Iinclude
ASFLAGS = --64
LDFLAGS = -T linker.ld -nostdlib -z max-page-size=0x1000

KERNEL = out/amdroid.elf
ISO = out/amdroid.iso

# Framebuffer mode requested from GRUB, as WIDTHxHEIGHTxBPP (bpp must be 32).
# Override on the command line, e.g.: make GFXMODE=1920x1080x32
GFXMODE ?= 1280x800x32

C_SRCS = \
 src/kernel/main.c \
 src/kernel/idt.c \
 src/kernel/memory/pmm.c \
 src/kernel/memory/heap.c \
 src/kernel/memory/vmm.c \
 src/kernel/fs/vfs.c \
 src/kernel/sched/sched.c \
 src/kernel/shell.c \
 src/kernel/drivers/serial.c \
 src/kernel/drivers/vga.c \
 src/kernel/drivers/pic.c \
 src/kernel/drivers/timer.c \
 src/kernel/drivers/keyboard.c \
 src/kernel/drivers/ata.c \
 src/kernel/drivers/fb.c \
 src/kernel/drivers/mouse.c \
 src/kernel/gui.c \
 src/kernel/fs/fat32.c \
 src/kernel/syscall.c \
 src/lib/console.c \
 src/lib/string.c \
 src/lib/klib.c \
 src/lib/kmath.c \
 src/kernel/fuss/front.c \
 src/kernel/fuss/eval.c \
 src/kernel/fuss/lib.c \
 src/kernel/fuss/run.c \
 src/kernel/fuss/gc.c \
 src/kernel/gdt.c \
 src/kernel/memory/vm.c \
 src/kernel/userdemo.c \
 src/kernel/elf.c \
 src/kernel/drivers/rtc.c \
 src/kernel/drivers/fbcon.c \
 src/kernel/auth.c \
 src/kernel/fs/ext2.c \
 src/kernel/drivers/pci.c \
 src/kernel/drivers/hda.c \
 src/kernel/drivers/e1000.c \
 src/kernel/drivers/virtio_net.c \
 src/kernel/drivers/xhci.c \
 src/kernel/net/nic.c \
 src/kernel/net/net.c \
 src/kernel/crypto/sha256.c

ASM_SRCS = \
 src/boot/boot.asm \
 src/boot/isr_stubs.asm \
 src/user/hello.asm \
 src/user/busy.asm

OBJS = $(C_SRCS:.c=.o) $(ASM_SRCS:.asm=.o)

all: $(ISO)

# Programs for the disk (run them with `run NAME.ELF` in the OS).
USER_PROGS = hello spin cat echo fwrite whoami sigtest
USER_ELFS = $(addprefix out/user/,$(addsuffix .elf,$(USER_PROGS)))

userprogs: $(USER_ELFS)

out/user/%.elf: src/user/c/%.c src/user/c/ulib.c src/user/c/ulib.h src/user/c/linker.ld
	@mkdir -p out/user
	$(CC) -std=gnu11 -ffreestanding -O2 -fno-stack-protector -fno-pie -mno-red-zone -m64 -Wall -c src/user/c/ulib.c -o out/user/ulib.o
	$(CC) -std=gnu11 -ffreestanding -O2 -fno-stack-protector -fno-pie -mno-red-zone -m64 -Wall -c $< -o out/user/$*.o
	$(LD) -T src/user/c/linker.ld -nostdlib -static -o $@ out/user/ulib.o out/user/$*.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# The interpreter is hot code: build it optimised.
src/kernel/fuss/%.o: CFLAGS += -O2

%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $(KERNEL)

$(ISO): $(KERNEL) grub.cfg
	mkdir -p out/iso/boot/grub
	cp $(KERNEL) out/iso/boot/amdroid.elf
	sed 's/^set gfxpayload=.*/set gfxpayload=$(GFXMODE)/' grub.cfg > out/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) out/iso 2>/dev/null

clean:
	rm -f $(OBJS) $(KERNEL) $(ISO)
	rm -rf out/iso

run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -serial stdio -m 256M

run-hd: $(KERNEL)
	qemu-system-x86_64 -kernel $(KERNEL) -serial stdio -m 256M

.PHONY: all clean run run-hd userprogs
