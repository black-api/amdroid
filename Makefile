CC = gcc
AS = as
LD = ld
CFLAGS = -std=gnu11 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-pie -mno-red-zone -m64 -Iinclude -MMD -MP
ASFLAGS = --64
LDFLAGS = -T linker.ld -nostdlib -z max-page-size=0x1000

KERNEL = out/amdroid.elf
ISO = out/amdroid.iso

# Framebuffer mode requested from GRUB, as WIDTHxHEIGHTxBPP (bpp must be 32).
# The QEMU window is as big as this mode, so it defaults to about 85% of the host screen
# (tools/screen-fit.sh): the window fits on the display instead of covering all of it.
# Override on the command line, e.g.: make GFXMODE=1920x1080x32
GFXMODE ?= $(shell sh tools/screen-fit.sh)

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
HUSSER_ENGINE = src/kernel/web/html.c src/kernel/web/css.c src/kernel/web/layout.c src/kernel/web/jsdom.c src/kernel/web/image.c src/kernel/web/svg.c src/kernel/web/webp.c src/kernel/js/js.c src/kernel/js/regex.c
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
 src/lib/markdown.c \
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
 src/kernel/drivers/rtl8169.c \
 src/kernel/drivers/rtl8139.c \
 src/kernel/drivers/xhci.c \
 src/kernel/net/nic.c \
 src/kernel/net/net.c \
 src/kernel/crypto/sha256.c \
 src/kernel/web/image.c \
 src/kernel/web/svg.c \
 src/kernel/web/webp.c \
 src/kernel/reel_bridge.c \
 src/kernel/scribe_bridge.c

ASM_SRCS = \
 src/boot/boot.asm \
 src/boot/isr_stubs.asm \
 src/user/hello.asm \
 src/user/busy.asm \
 src/user/husser/husser_blob.asm \
 src/user/reel/reel_blob.asm \
 src/user/scribe/scribe_blob.asm

OBJS = $(C_SRCS:.c=.o) $(ASM_SRCS:.asm=.o)

# RAM for the VM. 1G is enough for ultra mode (the browser's big caches and script heap).
MEM ?= 1G
# sound for MVP (the video player) and beep: an HD Audio card on PulseAudio
QEMU_AUDIO = -audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0
# QEMU's window: GTK, zoomed down to fit the screen if the guest mode is still bigger than
# it. `make run QEMU_DISPLAY="-display none -serial stdio"` for no window, `-display sdl`
# for SDL.
QEMU_DISPLAY ?= -display gtk,zoom-to-fit=on
# Hardware virtualization when the host has it (/dev/kvm usable): the OS runs at near
# native speed instead of being emulated instruction by instruction (TCG). `make run KVM=0`
# forces emulation. KVM needs VT-x/AMD-V switched on in the BIOS/UEFI and the kvm module
# loaded (sudo modprobe kvm_intel); `make kvm` says what this machine is missing.
KVM_OK := $(shell test -r /dev/kvm -a -w /dev/kvm && echo 1)
ifeq ($(KVM),0)
QEMU_ACCEL := -accel tcg,thread=multi
else ifeq ($(KVM_OK),1)
QEMU_ACCEL := -accel kvm -cpu host
else
QEMU_ACCEL := -accel tcg,thread=multi
endif

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

# Reel, the media process: demuxing (src/media), the vendored decoders and colour
# conversion, built with musl for ring 3 and embedded like Husser. The decoders come from
# tools/codecs/build.sh (FFmpeg's libavcodec, dav1d, musl; downloaded once, checksummed).
CODECS = out/codecs
CODECS_DONE = $(CODECS)/.done
REEL_CC = $(CODECS)/musl/bin/musl-gcc
REEL_CFLAGS = -std=gnu11 -O2 -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -w \
 -Iinclude -Isrc/media -I$(CODECS)/src/ffmpeg-7.1.2 -I$(CODECS)/ffbuild \
 -I$(CODECS)/src/dav1d-1.5.1/include -I$(CODECS)/src/dav1d-1.5.1/build/include
REEL_SRCS = src/user/reel/reel.c src/media/sbuf.c src/media/codec.c src/media/yuv.c
REEL_OBJS = $(addprefix out/reel/,$(notdir $(REEL_SRCS:.c=.o))) out/reel/glue.o
REEL_LIBS = $(CODECS)/lib/libavcodec.a $(CODECS)/lib/libswresample.a $(CODECS)/lib/libavutil.a \
 $(CODECS)/lib/libdav1d.a $(CODECS)/lib/libc-musl.a
LIBGCC := $(shell $(CC) -print-libgcc-file-name)

$(CODECS_DONE):
	bash tools/codecs/build.sh

out/reel/reel.o: src/user/reel/reel.c $(CODECS_DONE) include/amdroid/reel_proto.h src/media/media.h
	@mkdir -p out/reel
	$(REEL_CC) $(REEL_CFLAGS) -c $< -o $@
out/reel/%.o: src/media/%.c $(CODECS_DONE) src/media/media.h
	@mkdir -p out/reel
	$(REEL_CC) $(REEL_CFLAGS) -c $< -o $@
# the allocator replaces malloc: the compiler must not assume what malloc/free do
out/reel/glue.o: src/user/reel/glue.c $(CODECS_DONE)
	@mkdir -p out/reel
	$(REEL_CC) $(REEL_CFLAGS) -fno-builtin -ffreestanding -c $< -o $@

out/user/reel.elf: $(REEL_OBJS) $(CODECS_DONE) src/user/reel/linker.ld
	@mkdir -p out/user
	$(LD) -T src/user/reel/linker.ld -static -nostdlib --no-warn-rwx-segments -o $@ $(REEL_OBJS) $(REEL_LIBS) $(LIBGCC)

out/user/reel.bin: out/user/reel.elf
	objcopy -O binary $< $@

out/user/reel_defs.inc: out/user/reel.elf
	@nm $< | awk '$$NF=="__reel_end"{print ".global reel_bss_end\nreel_bss_end = 0x" $$1 ";"} $$NF=="_start"{print ".global reel_entry\nreel_entry = 0x" $$1 ";"}' > $@
	@grep -q reel_entry $@ || { echo "reel: _start not found"; exit 1; }
	@grep -q reel_bss_end $@ || { echo "reel: __reel_end not found"; exit 1; }

src/user/reel/reel_blob.o: src/user/reel/reel_blob.asm out/user/reel.bin out/user/reel_defs.inc

# Scribe, the speech-to-text process: the Thimble Whisper engine, built with Reel's musl
# toolchain, and its model (ThimbleLM/models/whisper-tiny-en.thim) embedded beside it.
SCRIBE_CFLAGS = -std=gnu11 -O3 -msse4.1 -fno-math-errno -fno-pie -fno-stack-protector \
 -fno-asynchronous-unwind-tables -w -Iinclude -IThimbleLM/engine/include
SCRIBE_OBJS = out/scribe/scribe.o out/scribe/stt.o out/scribe/needle.o out/scribe/glue.o
out/scribe/scribe.o: src/user/scribe/scribe.c include/amdroid/scribe_proto.h include/amdroid/voice_tools.h ThimbleLM/engine/include/thimble_needle.h $(CODECS_DONE)
	@mkdir -p out/scribe
	$(REEL_CC) $(SCRIBE_CFLAGS) -c $< -o $@
out/scribe/stt.o: ThimbleLM/engine/src/stt.c ThimbleLM/engine/include/thimble_stt.h $(CODECS_DONE)
	@mkdir -p out/scribe
	$(REEL_CC) $(SCRIBE_CFLAGS) -c $< -o $@
out/scribe/needle.o: ThimbleLM/engine/src/needle.c ThimbleLM/engine/include/thimble_needle.h $(CODECS_DONE)
	@mkdir -p out/scribe
	$(REEL_CC) $(SCRIBE_CFLAGS) -c $< -o $@
out/scribe/glue.o: src/user/scribe/glue.c src/user/reel/glue.c $(CODECS_DONE)
	@mkdir -p out/scribe
	$(REEL_CC) $(SCRIBE_CFLAGS) -fno-builtin -ffreestanding -c $< -o $@
out/user/scribe.elf: $(SCRIBE_OBJS) src/user/scribe/linker.ld
	@mkdir -p out/user
	$(LD) -T src/user/scribe/linker.ld -static -nostdlib --no-warn-rwx-segments -o $@ $(SCRIBE_OBJS) $(CODECS)/lib/libc-musl.a $(LIBGCC)
out/user/scribe.bin: out/user/scribe.elf
	objcopy -O binary $< $@
out/user/scribe_defs.inc: out/user/scribe.elf
	@nm $< | awk '$$NF=="__scribe_end"{print ".global scribe_bss_end\nscribe_bss_end = 0x" $$1 ";"} $$NF=="_start"{print ".global scribe_entry\nscribe_entry = 0x" $$1 ";"}' > $@
	@grep -q scribe_entry $@ || { echo "scribe: _start not found"; exit 1; }
src/user/scribe/scribe_blob.o: src/user/scribe/scribe_blob.asm out/user/scribe.bin out/user/scribe_defs.inc ThimbleLM/models/whisper-tiny-en.thim ThimbleLM/hf/needle3/needle3.cact

# The media pipeline on the host, against real files: make mediatest; out/mtest/mediatest FILE
mediatest: $(CODECS_DONE)
	@mkdir -p out/mtest
	$(REEL_CC) -O2 -static -no-pie -fno-pie $(filter-out -w,$(REEL_CFLAGS)) -Wall -o out/mtest/mediatest \
	  tools/harness/mediatest.c src/media/sbuf.c src/media/codec.c src/media/yuv.c \
	  $(CODECS)/lib/libavcodec.a $(CODECS)/lib/libswresample.a $(CODECS)/lib/libavutil.a $(CODECS)/lib/libdav1d.a -lm

userprogs: $(USER_ELFS)

$(KERNEL): $(OBJS) out/user/husser.bin out/user/husser_defs.inc out/user/reel.bin out/user/reel_defs.inc
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
# The video player's path: decoding frames, reading the disk, streaming sound.
src/kernel/web/image.o: CFLAGS += -O2
src/kernel/web/svg.o: CFLAGS += -O2
src/kernel/web/webp.o: CFLAGS += -O2
src/kernel/fs/fat32.o: CFLAGS += -O2
src/kernel/drivers/ata.o: CFLAGS += -O2
src/kernel/drivers/hda.o: CFLAGS += -O2
src/lib/kmath.o: CFLAGS += -O2
src/lib/markdown.o: CFLAGS += -O2
# the TCP/IP stack handles every packet: optimized (the rest of the kernel stays -O0)
src/kernel/net/net.o: CFLAGS += -O2
# Reel's kernel side: it copies the sound and the pictures for every frame
src/kernel/reel_bridge.o: CFLAGS += -O2

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
	qemu-system-x86_64 $(QEMU_ACCEL) $(QEMU_DISPLAY) -cdrom $(ISO) -serial stdio -m $(MEM) $(QEMU_AUDIO)

run-hd: $(KERNEL)
	qemu-system-x86_64 $(QEMU_ACCEL) $(QEMU_DISPLAY) -kernel $(KERNEL) -serial stdio -m $(MEM)

# ---- app packages (.adp) and a FAT32 disk image to install them from ----
# `make pkgs`  builds the sample packages in out/pkg/
# `make disk`  builds disk.img (FAT32) with the packages in its root
# `make run-disk` boots the ISO with that disk attached (so the App Store sees them)
PKG_OUT = out/pkg
ADP_FILES = $(PKG_OUT)/HELLO.ADP $(PKG_OUT)/BARS.ADP $(PKG_OUT)/SKETCHY.ADP $(PKG_OUT)/MDVIEW.ADP \
            $(PKG_OUT)/SNAKE.ADP $(PKG_OUT)/TILES.ADP $(PKG_OUT)/LIFE.ADP $(PKG_OUT)/STARS.ADP

# the newer apps: examples/apps/<lowercase id> -> out/pkg/<ID>.ADP
STORE_APPS = snake tiles life stars
$(foreach a,$(STORE_APPS),$(eval $(PKG_OUT)/$(shell echo $(a) | tr a-z A-Z).ADP: examples/apps/$(a)/app.manifest examples/apps/$(a)/MAIN.FAS tools/adp-build.py ; @mkdir -p $(PKG_OUT) && python3 tools/adp-build.py examples/apps/$(a) -o $$@))

$(PKG_OUT)/HELLO.ADP: examples/apps/hello/app.manifest examples/apps/hello/MAIN.FAS tools/adp-build.py
	@mkdir -p $(PKG_OUT)
	python3 tools/adp-build.py examples/apps/hello -o $@

$(PKG_OUT)/BARS.ADP: examples/apps/bars/app.manifest examples/apps/bars/MAIN.FAS tools/adp-build.py
	@mkdir -p $(PKG_OUT)
	python3 tools/adp-build.py examples/apps/bars -o $@

$(PKG_OUT)/SKETCHY.ADP: examples/apps/sketchy/app.manifest examples/apps/sketchy/MAIN.FAS tools/adp-build.py
	@mkdir -p $(PKG_OUT)
	python3 tools/adp-build.py examples/apps/sketchy -o $@

$(PKG_OUT)/MDVIEW.ADP: examples/apps/mdview/app.manifest examples/apps/mdview/MAIN.FAS tools/adp-build.py
	@mkdir -p $(PKG_OUT)
	python3 tools/adp-build.py examples/apps/mdview -o $@

# A deliberately corrupt package, to show the installer rejecting bad input.
$(PKG_OUT)/BROKEN.ADP: $(PKG_OUT)/HELLO.ADP
	head -c 200 $(PKG_OUT)/HELLO.ADP > $@

pkgs: $(ADP_FILES) $(PKG_OUT)/BROKEN.ADP

# The App Store server: the catalog and packages the App Store app downloads (from the guest,
# QEMU's user network reaches it at 10.0.2.2:8642). Apps with a store.meta are listed.
store-server:
	python3 tools/store-server.py

disk.img: $(ADP_FILES) $(PKG_OUT)/BROKEN.ADP
	mkfs.fat -C -F 32 -S 512 -s 1 $@ 65536
	mcopy -o -i $@ $(ADP_FILES) $(PKG_OUT)/BROKEN.ADP ::/

disk: disk.img

# format=raw is not cosmetic: with a guessed format QEMU refuses writes to the start of the
# image, which is where the FAT and its FSInfo sector live, so the OS could read the disk but
# never save a file (downloads, YouTube videos).
run-disk: $(ISO) disk.img
	qemu-system-x86_64 $(QEMU_ACCEL) $(QEMU_DISPLAY) -cdrom $(ISO) \
		-drive file=disk.img,format=raw,if=ide,index=0,media=disk \
		-boot d -serial stdio -m $(MEM) $(QEMU_AUDIO)

# Why the VM is fast or slow on this machine, and where to fix it: `make kvm`
kvm:
	@if [ "$(KVM_OK)" = 1 ]; then \
		echo "KVM: available - run/run-hd/run-disk use -accel kvm -cpu host"; \
	else \
		echo "KVM: not available - run/run-hd/run-disk emulate the CPU (TCG, slow but correct)"; \
		if grep -qw vmx /proc/cpuinfo || grep -qw svm /proc/cpuinfo; then \
			echo "  the CPU offers it: load the module with  sudo modprobe kvm_intel  (kvm_amd on AMD)"; \
		else \
			echo "  the CPU reports no vmx/svm: switch Virtualization Technology (VT-x/AMD-V) on in the BIOS/UEFI, save, reboot"; \
		fi; \
		lsmod 2>/dev/null | grep -qE '^kvm_(intel|amd)' && echo "  the kvm module is loaded but /dev/kvm is missing or not writable - check its permissions"; \
	fi; :

.PHONY: all clean run run-hd run-disk disk pkgs store-server userprogs kvm mediatest FORCE

# Header dependencies (written by -MMD): a changed header rebuilds the files that use it.
-include $(OBJS:.o=.d) $(HUSSER_OBJS:.o=.d)
