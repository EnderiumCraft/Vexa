# Vexa build system.
#   make          build build/vexa.iso
#   make run      boot the ISO in QEMU (window + serial log in the terminal)
#   make run-disk the same, with a disk that keeps its files between runs
#   make run-nographic   boot headless; serial log only (Ctrl-A X to quit)
#   make programs build the user programs in userland/ (with libvexa)
#   make test     boot in QEMU (BIOS; UEFI with 4 CPUs, 6 GiB and a modern CPU
#                 model; safe mode), type commands and check the replies
#   make clean

CC      ?= cc
LD      ?= ld
QEMU    ?= qemu-system-x86_64
LIMINE_BRANCH := v9.x-binary

BUILD   := build
KERNEL  := $(BUILD)/vexa-kernel
ISO     := $(BUILD)/vexa.iso
# Same kernel, but the boot menu defaults to safe mode. Used by `make test`.
SAFE_ISO := $(BUILD)/vexa-safe-mode-test.iso

CFLAGS := -g -O2 -pipe -std=gnu11 -Wall -Wextra -Werror \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
	-fno-PIC -fno-omit-frame-pointer -m64 -march=x86-64 \
	-mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel \
	-Ikernel/include -Iabi -MMD -MP
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	--no-dynamic-linker -T kernel/linker.ld

SRCS := $(shell find kernel/src -name '*.c' -o -name '*.S')
OBJS := $(patsubst kernel/src/%,$(BUILD)/obj/%.o,$(SRCS))

# User programs: libvexa plus one directory per program under userland/.
# -nostdinc keeps the host's C library headers out; only the compiler's own
# (stdint.h, stdarg.h...) and libvexa's are used.
USER_CFLAGS := -g -O2 -pipe -std=gnu11 -Wall -Wextra -Werror \
	-ffreestanding -fno-stack-protector -fno-PIC -fno-pie -m64 -march=x86-64 \
	-nostdinc -isystem $(shell $(CC) -print-file-name=include) \
	-Ilibvexa/include -Iabi -MMD -MP
USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	--no-dynamic-linker -T libvexa/program.ld

LIBVEXA_SRCS := $(wildcard libvexa/src/*.c libvexa/src/*.S)
LIBVEXA_OBJS := $(patsubst libvexa/src/%,$(BUILD)/libvexa/%.o,$(LIBVEXA_SRCS))
PROGRAMS := $(notdir $(wildcard userland/*))
PROGRAM_BINS := $(addprefix $(BUILD)/programs/,$(PROGRAMS))
INITRAMFS := $(BUILD)/initramfs.tar
ROOTFS_FILES := $(shell find rootfs -type f)

# Test disks for `make test`: the same ext2 file system (tests/disk-content
# plus hello-world) on three kinds of disk, each with a different layout.
DISK_CONTENT_FILES := $(shell find tests/disk-content -type f)
TEST_DISKS := $(BUILD)/disks/virtio-gpt.img $(BUILD)/disks/sata-mbr.img \
	$(BUILD)/disks/nvme-whole.img
# A disk for `make run-disk`, created once and kept, so changes survive reboots.
MY_DISK := $(BUILD)/my-disk.img
USER_OBJS := $(LIBVEXA_OBJS) \
	$(patsubst %,$(BUILD)/%.o,$(wildcard $(addsuffix /*.c,$(addprefix userland/,$(PROGRAMS)))))

.PHONY: all kernel programs iso run run-disk run-nographic test test-disks clean distclean

all: iso
kernel: $(KERNEL)
programs: $(PROGRAM_BINS)
iso: $(ISO)

$(BUILD)/obj/%.c.o: kernel/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.S.o: kernel/src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/libvexa/%.o: libvexa/src/%
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/userland/%.c.o: userland/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

.SECONDEXPANSION:
$(BUILD)/programs/%: $(LIBVEXA_OBJS) libvexa/program.ld \
		$$(addprefix $(BUILD)/,$$(addsuffix .o,$$(wildcard userland/$$*/*.c)))
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(filter %.o,$^) -o $@

# The starting root file system: rootfs/ plus the programs in /bin, as a tar
# archive (ustar, with fixed owners and times so builds are reproducible).
$(INITRAMFS): $(PROGRAM_BINS) $(ROOTFS_FILES)
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin
	cp -R rootfs/. $(BUILD)/rootfs/
	cp $(PROGRAM_BINS) $(BUILD)/rootfs/bin/
	tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime=@0 --sort=name \
		-cf $@ -C $(BUILD)/rootfs .

$(BUILD)/disk-content: $(DISK_CONTENT_FILES) $(BUILD)/programs/hello-world
	rm -rf $@
	cp -R tests/disk-content $@
	cp $(BUILD)/programs/hello-world $@/

$(BUILD)/disks/virtio-gpt.img: $(BUILD)/disk-content tools/make-disk.py
	@mkdir -p $(dir $@)
	tools/make-disk.py $@ 32 gpt $<

$(BUILD)/disks/sata-mbr.img: $(BUILD)/disk-content tools/make-disk.py
	@mkdir -p $(dir $@)
	tools/make-disk.py $@ 24 mbr $<

$(BUILD)/disks/nvme-whole.img: $(BUILD)/disk-content tools/make-disk.py
	@mkdir -p $(dir $@)
	tools/make-disk.py $@ 16 none $<

test-disks: $(TEST_DISKS)

$(MY_DISK): | $(BUILD)/disks/virtio-gpt.img
	cp $(BUILD)/disks/virtio-gpt.img $@

# The boot menu loads the initramfs next to the kernel in every entry.
$(BUILD)/limine.conf: limine.conf Makefile
	@mkdir -p $(BUILD)
	awk '{ print } /^ *path: boot\(\):\/boot\/vexa-kernel/ { \
		print "    module_path: boot():/boot/initramfs.tar" }' limine.conf > $@

limine/limine:
	rm -rf limine
	git clone https://github.com/limine-bootloader/limine.git --branch=$(LIMINE_BRANCH) --depth=1 limine
	$(MAKE) -C limine

# $(call make_iso,limine config file,output ISO)
define make_iso
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/
	cp $(INITRAMFS) $(BUILD)/iso_root/boot/
	cp $(1) $(BUILD)/iso_root/boot/limine/limine.conf
	cp limine/limine-bios.sys limine/limine-bios-cd.bin \
		limine/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp limine/BOOTX64.EFI limine/BOOTIA32.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BUILD)/iso_root -o $(2) 2>/dev/null
	./limine/limine bios-install $(2)
endef

$(ISO): $(KERNEL) $(INITRAMFS) $(BUILD)/limine.conf limine/limine
	$(call make_iso,$(BUILD)/limine.conf,$@)

$(SAFE_ISO): $(KERNEL) $(INITRAMFS) $(BUILD)/limine.conf limine/limine
	{ echo 'default_entry: 2'; cat $(BUILD)/limine.conf; } > $(BUILD)/limine-safe-mode.conf
	$(call make_iso,$(BUILD)/limine-safe-mode.conf,$@)

run: $(ISO)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -serial stdio -no-reboot

# Like run, with a virtio disk mounted at /mnt/vda1. It keeps what you write;
# delete build/my-disk.img to start over.
run-disk: $(ISO) $(MY_DISK)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -boot d -serial stdio -no-reboot \
		-drive file=$(MY_DISK),if=virtio,format=raw

run-nographic: $(ISO)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -nographic -no-reboot

test: $(ISO) $(SAFE_ISO) $(TEST_DISKS)
	tools/qemu-smoke-test.py --disks $(BUILD)/disks
	tools/qemu-smoke-test.py --disks $(BUILD)/disks --uefi --smp 4 --memory 6G --cpu max
	tools/qemu-smoke-test.py --disks $(BUILD)/disks --safe-mode --iso $(SAFE_ISO)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf limine

-include $(OBJS:.o=.d) $(USER_OBJS:.o=.d)
