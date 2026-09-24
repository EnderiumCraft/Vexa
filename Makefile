# Vexa build system.
#   make          build build/vexa.iso
#   make run      boot the ISO in QEMU (window + serial log in the terminal)
#   make run-nographic   boot headless; serial log only (Ctrl-A X to quit)
#   make test     boot in QEMU (BIOS, UEFI with 4 CPUs, and without ACPI) and
#                 check it responds at the keyboard
#   make clean

CC      ?= cc
LD      ?= ld
QEMU    ?= qemu-system-x86_64
LIMINE_BRANCH := v9.x-binary

BUILD   := build
KERNEL  := $(BUILD)/vexa-kernel
ISO     := $(BUILD)/vexa.iso

CFLAGS := -g -O2 -pipe -std=gnu11 -Wall -Wextra -Werror \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
	-fno-PIC -fno-omit-frame-pointer -m64 -march=x86-64 \
	-mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel \
	-Ikernel/include -MMD -MP
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	--no-dynamic-linker -T kernel/linker.ld

SRCS := $(shell find kernel/src -name '*.c' -o -name '*.S')
OBJS := $(patsubst kernel/src/%,$(BUILD)/obj/%.o,$(SRCS))

.PHONY: all kernel iso run run-nographic test clean distclean

all: iso
kernel: $(KERNEL)
iso: $(ISO)

$(BUILD)/obj/%.c.o: kernel/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.S.o: kernel/src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

limine/limine:
	rm -rf limine
	git clone https://github.com/limine-bootloader/limine.git --branch=$(LIMINE_BRANCH) --depth=1 limine
	$(MAKE) -C limine

$(ISO): $(KERNEL) limine.conf limine/limine
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/
	cp limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin \
		limine/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp limine/BOOTX64.EFI limine/BOOTIA32.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BUILD)/iso_root -o $@ 2>/dev/null
	./limine/limine bios-install $@

run: $(ISO)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -serial stdio -no-reboot

run-nographic: $(ISO)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -nographic -no-reboot

test: $(ISO)
	tools/qemu-smoke-test.py
	tools/qemu-smoke-test.py --uefi --smp 4
	tools/qemu-smoke-test.py --safe-mode

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf limine

-include $(OBJS:.o=.d)
