CC = gcc
AS = as
LD = ld
CFLAGS = -std=gnu11 -ffreestanding -O0 -Wall -Wextra -fno-stack-protector -fno-pie -mno-red-zone -m64 -Iinclude -MMD -MP
ASFLAGS = --64
LDFLAGS = -T linker.ld -nostdlib -z max-page-size=0x1000

KERNEL = out/amdroid.elf
ISO = out/amdroid.iso

# Framebuffer mode requested from GRUB, as WIDTHxHEIGHTxBPP (bpp must be 32).
# Override on the command line, e.g.: make GFXMODE=1920x1080x32
GFXMODE ?= 1920x1080x32

# The kernel forces this exact mode on QEMU's VGA; keep it in sync with GFXMODE.
GFX_W := $(word 1,$(subst x, ,$(GFXMODE)))
GFX_H := $(word 2,$(subst x, ,$(GFXMODE)))
CFLAGS += -DFB_W=$(GFX_W) -DFB_H=$(GFX_H)

# make PERF_LOG=1: the GUI writes its redraw counts and times to the serial port every second.
ifdef PERF_LOG
CFLAGS += -DGUI_PERF_LOG
endif

# GUI: the core in src/kernel/gui/, and every app in apps/.
GUI_SRCS = $(wildcard src/kernel/gui/*.c) $(wildcard apps/*.c)

# The Husser renderer: the web engine (HTML, CSS, layout, images) and the JavaScript
# interpreter, built for ring 3 as a blob that the kernel embeds and starts as a process.
HUSSER_ENGINE = src/kernel/web/html.c src/kernel/web/css.c src/kernel/web/layout.c src/kernel/web/jsdom.c src/kernel/web/image.c src/kernel/js/js.c src/kernel/js/regex.c
HUSSER_SRCS = src/user/husser/husser.c src/user/husser/heap_shim.c src/user/husser/painter.c $(HUSSER_ENGINE) src/lib/string.c src/lib/klib.c src/lib/kmath.c
HUSSER_OBJS = $(addprefix out/husser/,$(notdir $(HUSSER_SRCS:.c=.o)))
HUSSER_CFLAGS = -std=gnu11 -ffreestanding -O2 -fno-stack-protector -fno-pie -mno-red-zone -m64 -w -Isrc/user/husser/shim -Iinclude -MMD -MP

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
 $(GUI_SRCS) \
 src/kernel/fs/fat32.c \
 src/kernel/pkg/pkg.c \
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
 src/kernel/husser_bridge.c \
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
 src/user/busy.asm \
 src/user/husser/husser_blob.asm

OBJS = $(C_SRCS:.c=.o) $(ASM_SRCS:.asm=.o)

all: $(ISO)

# Programs for the disk (run them with `run NAME.ELF` in the OS).
USER_PROGS = hello spin cat echo fwrite whoami sigtest
USER_ELFS = $(addprefix out/user/,$(addsuffix .elf,$(USER_PROGS)))

# The husser blob must exist before its asm file is assembled.
src/user/husser/husser_blob.o: src/user/husser/husser_blob.asm out/user/husser.bin out/user/husser_defs.inc

# The husser renderer: ET_EXEC at VM_USER_BASE, flattened for embedding.
vpath %.c src/kernel/web src/kernel/js src/lib src/user/husser
out/husser/%.o: %.c
	@mkdir -p out/husser
	$(CC) $(HUSSER_CFLAGS) -c $< -o $@

out/user/husser.elf: $(HUSSER_OBJS) src/user/husser/linker.ld
	@mkdir -p out/user
	$(LD) -T src/user/husser/linker.ld -nostdlib -static -o $@ $(HUSSER_OBJS)

# verify the entry and flatten the image
out/user/husser.bin: out/user/husser.elf
	objcopy -O binary $< $@

out/user/husser_defs.inc: out/user/husser.elf
	@nm $< | awk '$$NF=="__husser_end"{print ".global husser_bss_end\nhusser_bss_end = 0x" $$1 ";"} $$NF=="_start"{print ".global husser_entry\nhusser_entry = 0x" $$1 ";"}' > $@
	@grep -q husser_entry $@ || { echo "husser: _start not found"; exit 1; }
	@test -s $@ || { echo "husser: __husser_end not found"; exit 1; }

userprogs: $(USER_ELFS)

$(KERNEL): $(OBJS) out/user/husser.bin out/user/husser_defs.inc
	$(LD) $(LDFLAGS) $(OBJS) -o $(KERNEL)

out/user/%.elf: src/user/c/%.c src/user/c/ulib.c src/user/c/ulib.h src/user/c/linker.ld
	@mkdir -p out/user
	$(CC) -std=gnu11 -ffreestanding -O2 -fno-stack-protector -fno-pie -mno-red-zone -m64 -Wall -c src/user/c/ulib.c -o out/user/ulib.o
	$(CC) -std=gnu11 -ffreestanding -O2 -fno-stack-protector -fno-pie -mno-red-zone -m64 -Wall -c $< -o out/user/$*.o
	$(LD) -T src/user/c/linker.ld -nostdlib -static -o $@ out/user/ulib.o out/user/$*.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# The interpreter is hot code: build it optimised.
src/kernel/fuss/%.o: CFLAGS += -O2

# Drawing code is hot too.
src/kernel/gui/%.o: CFLAGS += -O2
apps/%.o: CFLAGS += -O2
src/kernel/drivers/fb.o: CFLAGS += -O2

# A change of build options (GFXMODE, PERF_LOG) rebuilds what the old ones compiled.
FLAGS_STAMP = out/.flags
$(FLAGS_STAMP): FORCE
	@mkdir -p out
	@echo '$(CFLAGS)' | cmp -s - $@ || echo '$(CFLAGS)' > $@
$(OBJS): $(FLAGS_STAMP)
FORCE:

%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

$(ISO): $(KERNEL) grub.cfg
	mkdir -p out/iso/boot/grub
	cp $(KERNEL) out/iso/boot/amdroid.elf
	sed 's/^set gfxpayload=.*/set gfxpayload=$(GFXMODE)/' grub.cfg > out/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) out/iso 2>/dev/null

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(KERNEL) $(ISO) $(FLAGS_STAMP)
	rm -rf out/iso out/husser out/user/husser.elf out/user/husser.bin out/user/husser_defs.inc

run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -serial stdio -m 256M

run-hd: $(KERNEL)
	qemu-system-x86_64 -kernel $(KERNEL) -serial stdio -m 256M

# ---- app packages (.adp) and a FAT32 disk image to install them from ----
# `make pkgs`  builds the sample packages in out/pkg/
# `make disk`  builds disk.img (FAT32) with the packages in its root
# `make run-disk` boots the ISO with that disk attached (so the App Store sees them)
PKG_OUT = out/pkg
ADP_FILES = $(PKG_OUT)/HELLO.ADP $(PKG_OUT)/BARS.ADP $(PKG_OUT)/SKETCHY.ADP

$(PKG_OUT)/HELLO.ADP: examples/apps/hello/app.manifest examples/apps/hello/MAIN.FAS tools/adp-build.py
	@mkdir -p $(PKG_OUT)
	python3 tools/adp-build.py examples/apps/hello -o $@

$(PKG_OUT)/BARS.ADP: examples/apps/bars/app.manifest examples/apps/bars/MAIN.FAS tools/adp-build.py
	@mkdir -p $(PKG_OUT)
	python3 tools/adp-build.py examples/apps/bars -o $@

$(PKG_OUT)/SKETCHY.ADP: examples/apps/sketchy/app.manifest examples/apps/sketchy/MAIN.FAS tools/adp-build.py
	@mkdir -p $(PKG_OUT)
	python3 tools/adp-build.py examples/apps/sketchy -o $@

# A deliberately corrupt package, to show the installer rejecting bad input.
$(PKG_OUT)/BROKEN.ADP: $(PKG_OUT)/HELLO.ADP
	head -c 200 $(PKG_OUT)/HELLO.ADP > $@

pkgs: $(ADP_FILES) $(PKG_OUT)/BROKEN.ADP

disk.img: $(ADP_FILES) $(PKG_OUT)/BROKEN.ADP
	mkfs.fat -C -F 32 -S 512 -s 1 $@ 65536
	mcopy -o -i $@ $(ADP_FILES) $(PKG_OUT)/BROKEN.ADP ::/

disk: disk.img

run-disk: $(ISO) disk.img
	qemu-system-x86_64 -cdrom $(ISO) -hda disk.img -boot d -serial stdio -m 256M

.PHONY: all clean run run-hd run-disk disk pkgs userprogs FORCE

# Header dependencies (written by -MMD): a changed header rebuilds the files that use it.
-include $(OBJS:.o=.d) $(HUSSER_OBJS:.o=.d)
