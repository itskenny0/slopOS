# PicoOS build
CC      := gcc
LD      := ld
# `make PRO=1` builds PicoOS Pro v2.1: every program on the disk (stars and
# pong included), the modern Menu desktop, and user-facing PicoOS image names.
PROCFLAGS := $(if $(PRO),-DPICO_PRO,)
# The Pro build has a release name matching the visible PicoOS version.
IMG       := $(if $(PRO),PicoOS-Pro-2.1,PicoOS-2.1)

CFLAGS  := -m32 -march=i386 -mtune=i386 -std=gnu99 -ffreestanding -fno-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-builtin -nostdlib -nostdinc \
           -Wall -Wextra -Os -Iinclude $(PROCFLAGS)
ASFLAGS := -m32 -c
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib --build-id=none

BUILD   := build
CSRC    := $(wildcard kernel/*.c)
NSRC    := $(if $(wildcard net/*.c),$(wildcard net/*.c),stubs/net_stub.c)
OBJ     := $(BUILD)/entry.o $(BUILD)/vgaintr.o $(BUILD)/isr.o $(BUILD)/jmp.o $(BUILD)/switch.o \
           $(patsubst kernel/%.c,$(BUILD)/%.o,$(CSRC)) \
           $(patsubst %.c,$(BUILD)/%.o,$(NSRC)) \
           $(BUILD)/apps_data.o

# ---- .pico programs --------------------------------------------------
# Built with the same 386 flags as the kernel, but linked flat at 0x30000
# against apps/picoapp.h instead of the kernel headers. They end up baked
# into the kernel image as ramdisk files.
# The graphical desktop is a normal .pico program, bundled into the store
# and started by kernel/main.c. Keep every useful app in the daily-use image;
# only the old desktop variants and the intentional crash demo stay out.
APP_SRC   := $(filter-out apps/crt0.c apps/desk.c apps/gdesk.c apps/win95.c apps/crash.c,$(wildcard apps/*.c))
APP_NAMES := $(patsubst apps/%.c,%,$(APP_SRC))
APP_PICO  := $(patsubst %,$(BUILD)/apps/%.pico,$(APP_NAMES))
APP_CFLAGS := -m32 -march=i386 -mtune=i386 -std=gnu99 -ffreestanding -fno-pie \
              -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin \
              -nostdlib -nostdinc -Wall -Wextra -Os -Iapps $(PROCFLAGS)

# The UEFI loader needs no gnu-efi: efi/uefi.h declares the firmware
# interface itself, and the whole loader runs in the firmware's calling
# convention (-mabi=ms), so calls need no wrappers either.
EFI_CFLAGS := -Iefi -fpic -ffreestanding -fno-stack-protector -fno-strict-aliasing \
              -fno-asynchronous-unwind-tables -fno-ident \
              -fshort-wchar -mno-red-zone -mabi=ms -Wall

.PHONY: all apps clean

# The names are the documentation. Somebody wrote the UEFI-only image to a USB
# stick, booted a BIOS machine with it, got a blinking cursor and reasonably
# concluded the OS was broken -- that image is GPT with an EFI partition and
# nothing for a BIOS to start. The one to write to a stick is now called
# exactly that, and the special case is off in extra/.
# Two files to ship: a stick/hard-disk image and a CD. The 64 MiB FAT32
# variants (picoos-usb.img, extra/*-uefi-only.img) still build on request,
# they are just not made by default.
all: $(IMG)-usb-small.img $(IMG).img $(IMG).iso PicoOS-USB.img

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/net:
	@mkdir -p $(BUILD)/net

$(BUILD)/net/%.o: net/%.c | $(BUILD)/net
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/stubs:
	@mkdir -p $(BUILD)/stubs

$(BUILD)/stubs/%.o: stubs/%.c | $(BUILD)/stubs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@

.PRECIOUS: $(BUILD)/apps/%.o $(BUILD)/apps/%.elf $(BUILD)/apps/%.bin

# ---- programs --------------------------------------------------------
$(BUILD)/apps:
	@mkdir -p $(BUILD)/apps

$(BUILD)/apps/crt0.o: apps/crt0.c apps/picoapp.h | $(BUILD)/apps
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/apps/%.o: apps/%.c apps/picoapp.h | $(BUILD)/apps
	$(CC) $(APP_CFLAGS) -c $< -o $@

# -q keeps the relocation records in the ELF; mkpico.py turns them into the
# list that lets the kernel load a program at any address
$(BUILD)/apps/%.elf: $(BUILD)/apps/crt0.o $(BUILD)/apps/%.o apps/app.ld
	$(LD) -m elf_i386 -T apps/app.ld -nostdlib --build-id=none \
	      --no-warn-rwx-segments -q \
	      -o $@ $(BUILD)/apps/crt0.o $(BUILD)/apps/$*.o

$(BUILD)/apps/%.bin: $(BUILD)/apps/%.elf
	objcopy -O binary $< $@

$(BUILD)/apps/%.pico: $(BUILD)/apps/%.elf $(BUILD)/apps/%.bin tools/mkpico.py
	@python3 tools/mkpico.py $(BUILD)/apps/$*.elf $(BUILD)/apps/$*.bin $@ nm readelf

apps: $(APP_PICO)

# ---- DOOM: 78 upstream doomgeneric files plus the PicoOS platform layer
# in apps/doom/pico (input, screen, libc, fixed-point, system glue).
# doom.pico stays out of the kernel store -- it is half a megabyte and the
# floppy image has no room -- and ships on the -doom image with the WAD.
DOOM_CFLAGS := -m32 -march=i386 -mtune=i386 -std=gnu99 -ffreestanding -fno-pie \
              -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin \
              -nostdlib -nostdinc -Wall -Os -Iapps -Iapps/doom/src -Iapps/doom/libc \
              -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNDEBUG \
              -Wno-unused-function -Wno-unused-variable $(PROCFLAGS)
DOOM_SRC := $(wildcard apps/doom/src/*.c) $(wildcard apps/doom/pico/*.c)
DOOM_OBJ := $(patsubst apps/doom/%.c,$(BUILD)/doom/%.o,$(DOOM_SRC))

.PRECIOUS: $(BUILD)/doom/%.o $(BUILD)/doom.elf $(BUILD)/doom.bin

$(BUILD)/doom $(BUILD)/doom/src $(BUILD)/doom/pico:
	@mkdir -p $@

$(BUILD)/doom/%.o: apps/doom/%.c | $(BUILD)/doom $(BUILD)/doom/src $(BUILD)/doom/pico
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

$(BUILD)/doom.elf: $(BUILD)/apps/crt0.o $(DOOM_OBJ) apps/app.ld
	$(LD) -m elf_i386 -T apps/app.ld -nostdlib --build-id=none \
	      --no-warn-rwx-segments -q \
	      -o $@ $(BUILD)/apps/crt0.o $(DOOM_OBJ)

$(BUILD)/doom.bin: $(BUILD)/doom.elf
	objcopy -O binary $< $@

$(BUILD)/doom.pico: $(BUILD)/doom.elf $(BUILD)/doom.bin tools/mkpico.py
	@python3 tools/mkpico.py $(BUILD)/doom.elf $(BUILD)/doom.bin $@ nm readelf

doom: $(BUILD)/doom.pico

# the programs, turned into C arrays the kernel links in
$(BUILD)/apps_data.c: $(APP_PICO) tools/mkapps.py
	@python3 tools/mkapps.py $@ $(APP_PICO)

$(BUILD)/apps_data.o: $(BUILD)/apps_data.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

# --set-section-flags makes objcopy emit zeroes for .bss, so the loader
# allocates (and the boot sector loads) space for it too
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	objcopy -O binary --set-section-flags .bss=alloc,load,contents $< $@
	@sz=$$(stat -c%s $@); limit=$$((0x80000 - 0x10000)); \
	 if [ $$sz -gt $$limit ]; then \
	   echo "*** kernel is $$sz bytes, the limit is $$limit ***"; \
	   rm -f $@; exit 1; \
	 else \
	   echo "kernel      : $$sz bytes, $$((limit - sz)) bytes below the loader ceiling"; \
	 fi

$(BUILD)/boot.o: boot/boot.S | $(BUILD)
	$(CC) -m32 -c $< -o $@

$(BUILD)/boot.elf: $(BUILD)/boot.o
	$(LD) -m elf_i386 -Ttext 0x7C00 -o $@ $<

$(BUILD)/boot.bin: $(BUILD)/boot.elf
	objcopy -O binary $< $@

$(IMG).img: $(BUILD)/boot.bin $(BUILD)/kernel.bin
	python3 tools/mkimage.py $(BUILD)/boot.bin $(BUILD)/kernel.bin $(IMG).img $(BUILD)/boot.elf

# Where the shareware WAD lives for the images below (or point DOOM_WAD
# at it). Every USB image and the ISO carry DOOM: doom.pico plus this WAD
# in the app archive, so the game is preinstalled and listed under APPS.
#
# (There used to be a separate $(IMG)-doom.img here. It is gone: a 16-bit
# boot sector cannot load 4.6 MB -- it wraps at 1 MB and wipes itself --
# so that disk never booted. The USB images and the ISO are the DOOM
# carriers now; they load the archive above 1 MB through the UEFI loader.)
DOOM_WAD ?= doom1.wad

clean:
	rm -rf $(BUILD) picoos*.img picoos*.iso PicoOS-*.img PicoOS-*.iso extra

# ---- UEFI loader (no gnu-efi: linked straight to a PE32+ application)
$(BUILD)/loader.o: efi/loader.c efi/uefi.h | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/nolib.o: efi/nolib.c efi/uefi.h | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c $< -o $@

$(BUILD)/efi_trampoline.o: efi/trampoline.S | $(BUILD)
	$(CC) -c $< -o $@

# The loader is fully position-independent (verified: no absolute
# addressing anywhere), so the firmware can park it at any address with
# no base relocations to apply.
$(BUILD)/BOOTX64.EFI: $(BUILD)/loader.o $(BUILD)/nolib.o $(BUILD)/efi_trampoline.o
	$(LD) -m i386pep --oformat pei-x86-64 -pie --subsystem 10 \
	  --entry efi_main --image-base 0 -o $@ $^

# one image that boots on a 486 through the boot sector and on a modern
# machine through the EFI system partition
$(IMG)-usb.img: $(BUILD)/boot.bin $(BUILD)/kernel.bin $(BUILD)/BOOTX64.EFI \
  $(BUILD)/doom.pico
	python3 tools/mkhybrid.py $(BUILD)/boot.bin $(BUILD)/kernel.bin \
	  $(BUILD)/BOOTX64.EFI $(IMG)-usb.img $(BUILD)/boot.elf
	if [ -f "$(DOOM_WAD)" ]; then \
	  python3 tools/addapp.py $(IMG)-usb.img --esp-only \
	    $(BUILD)/doom.pico $(DOOM_WAD); \
	else echo "(no $(DOOM_WAD): USB image ships without DOOM)"; fi

# The same stick with an 8 MiB FAT16 partition instead of a 64 MiB FAT32 one:
# 9 MB to write instead of 65, which is the difference between landing and
# not on a stick that has started refusing writes part of the way through.
#
# Note: an older comment here claimed OVMF will not boot a FAT16 ESP --
# that was measured while the UEFI loader itself crashed on startup (its
# ST/BS globals went through a broken GOT indirection; fixed by marking
# them hidden so the compiler emits direct accesses). With the fixed
# loader this image boots fine under both BIOS and UEFI/OVMF. The 65 MB
# FAT32 one remains for real UEFI-only hardware with picky firmware.
$(IMG)-usb-small.img: $(BUILD)/boot.bin $(BUILD)/kernel.bin $(BUILD)/BOOTX64.EFI \
  $(BUILD)/doom.pico
	PICO_ESP_SECTORS=16384 PICO_ESP_FAT=16 python3 tools/mkhybrid.py \
	  $(BUILD)/boot.bin $(BUILD)/kernel.bin $(BUILD)/BOOTX64.EFI \
	  $(IMG)-usb-small.img $(BUILD)/boot.elf
	if [ -f "$(DOOM_WAD)" ]; then \
	  python3 tools/addapp.py $(IMG)-usb-small.img --esp-only \
	    $(BUILD)/doom.pico $(DOOM_WAD); \
	else echo "(no $(DOOM_WAD): USB image ships without DOOM)"; fi

# the user-facing copy of the small stick, under a name that says it
PicoOS-USB.img: $(IMG)-usb-small.img
	cp $< $@

extra/$(IMG)-uefi-only.img: $(BUILD)/kernel.bin $(BUILD)/BOOTX64.EFI tools/mkgpt.py
	@mkdir -p extra
	python3 tools/mkgpt.py $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $@

# a CD that boots both ways: El Torito floppy emulation for BIOS, and the
# EFI system partition as an alternative boot image for UEFI
$(BUILD)/isohbr.o: boot/isohbr.S | $(BUILD)
	$(CC) -m32 -c $< -o $@

$(BUILD)/isohbr.elf: $(BUILD)/isohbr.o
	$(LD) -m elf_i386 -Ttext 0x7C00 -o $@ $<

$(BUILD)/isohbr.bin: $(BUILD)/isohbr.elf $(BUILD)/kernel.bin
	objcopy -O binary $< $@
	python3 tools/mkisombr.py $@ $(BUILD)/isohbr.elf $(BUILD)/kernel.bin

# The EFI half of the CD: a small FAT image carrying the loader and the
# kernel, the way the big dd-out of the usb image did, without the 64 MiB.
# The kernel the ISO boots: padded to a sector, plus the app archive with
# DOOM and its WAD, so the CD carries the game the way the USB images do.
# The archive starts at the first sector boundary after the kernel, which
# is exactly where the kernel looks for it (see kernel/papp.c).
$(BUILD)/kern_doom.bin: $(BUILD)/kernel.bin $(BUILD)/doom.pico $(DOOM_WAD)
	@cp $(BUILD)/kernel.bin $(BUILD)/kern.pad
	@truncate -s %512 $(BUILD)/kern.pad
	python3 -c "import sys; sys.path.insert(0, 'tools'); from addapp import pack; k = open('$(BUILD)/kern.pad', 'rb').read(); a = pack([('doom.pico', open('$(BUILD)/doom.pico', 'rb').read()), ('doom1.wad', open('$(DOOM_WAD)', 'rb').read())]); open('$@', 'wb').write(k + a)"
$(BUILD)/efi.img: $(BUILD)/BOOTX64.EFI $(BUILD)/kern_doom.bin
	python3 tools/mkesp.py $@ 16384 EFI/BOOT/BOOTX64.EFI=$(BUILD)/BOOTX64.EFI \
	  picoos/kernel.bin=$(BUILD)/kern_doom.bin

$(IMG).iso: $(IMG).img $(BUILD)/efi.img $(BUILD)/isohbr.bin
	@mkdir -p $(BUILD)/iso && cp $(IMG).img $(BUILD)/iso/
	@cp $(BUILD)/efi.img $(BUILD)/iso/efi.img
	@xorrisofs -quiet -o $(IMG).iso -V PICOOS \
	  -b $(IMG).img -c boot.cat \
	  -eltorito-alt-boot -e efi.img -no-emul-boot \
	  -isohybrid-mbr $(BUILD)/isohbr.bin -isohybrid-gpt-basdat \
	  $(BUILD)/iso/ \
	  || echo "(xorriso missing -- skipping the ISO, the .img files still work)"
	@python3 tools/mkisoesp.py $(IMG).iso


# boot it in an emulator
run: $(IMG).img
	qemu-system-i386 -drive file=$(IMG).img,format=raw,if=floppy -m 8

run-hd: $(IMG).img
	qemu-system-i386 -drive file=$(IMG).img,format=raw,if=ide -boot c -m 8

run-iso: $(IMG).iso
	qemu-system-i386 -cdrom $(IMG).iso -boot d -m 8

# the machine this thing was actually designed for
run-486: $(IMG).img
	qemu-system-i386 -cpu 486 -m 2 -drive file=$(IMG).img,format=raw,if=floppy

# needs OVMF installed (package ovmf)
run-uefi: picoos-hybrid.img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
	  -drive file=picoos-hybrid.img,format=raw,if=ide -m 256

.PHONY: all clean run run-hd run-iso run-486 run-uefi

# vgaintr.o must stay within the first 64 KB after 0x10000: the 16-bit code
# segment in vgaintr.S has base 0x10000 and reaches exactly that far.
# It is second in OBJ on purpose; do not move it down the list.
