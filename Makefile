# Vexa build system.
#   make          build build/vexa.iso
#   make run      boot the ISO in QEMU (window + serial log in the terminal)
#   make run-disk the same, with a disk that keeps its files between runs
#   make run-nographic   boot headless; serial log only (Ctrl-A X to quit)
#   make programs build the user programs in userland/ (with libvexa)
#   make test     boot in QEMU (BIOS; UEFI with 4 CPUs, 6 GiB and a modern CPU
#                 model; safe mode; a kernel without the Linux subsystem), type
#                 commands and check the replies
#   make busybox  build BusyBox (needs musl-gcc: Debian/Ubuntu package musl-tools)
#   make clean

CC      ?= cc
LD      ?= ld
QEMU    ?= qemu-system-x86_64
# A network card behind QEMU's NAT (the guest gets 10.0.2.15 by DHCP).
QEMU_NET ?= -netdev user,id=net0 -device virtio-net-pci,netdev=net0
# A sound card (HD Audio), played through PulseAudio (or PipeWire's stand-in
# for it) on Linux and Core Audio on macOS. `make run QEMU_AUDIO=` leaves it out.
QEMU_AUDIO_DRIVER ?= $(if $(filter Darwin,$(shell uname -s)),coreaudio,pa)
QEMU_AUDIO ?= -audiodev $(QEMU_AUDIO_DRIVER),id=snd0 -device intel-hda -device hda-output,audiodev=snd0
# A USB controller with a tablet: the pointer follows the host's, without
# QEMU's window grabbing the mouse. `make run QEMU_USB=` leaves it out.
QEMU_USB ?= -device qemu-xhci,id=xhci -device usb-tablet,bus=xhci.0
LIMINE_BRANCH := v9.x-binary

BUILD   := build
KERNEL  := $(BUILD)/vexa-kernel
ISO     := $(BUILD)/vexa.iso
# Same kernel, but the boot menu defaults to safe mode. Used by `make test`.
SAFE_ISO := $(BUILD)/vexa-safe-mode-test.iso

# The Linux subsystem (kernel/src/personality/linux) is optional:
# `make LINUX_COMPAT=0` builds a kernel that runs only Vexa programs.
LINUX_COMPAT ?= 1

CFLAGS := -g -O2 -pipe -std=gnu11 -Wall -Wextra -Werror \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
	-fno-PIC -fno-omit-frame-pointer -m64 -march=x86-64 \
	-mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel \
	-Ikernel/include -Iabi -MMD -MP
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	--no-dynamic-linker -T kernel/linker.ld

SRCS := $(shell find kernel/src -name '*.c' -o -name '*.S')
ifeq ($(LINUX_COMPAT),1)
CFLAGS += -DLINUX_COMPAT
else
SRCS := $(filter-out kernel/src/personality/linux/%,$(SRCS))
endif
OBJS := $(patsubst kernel/src/%,$(BUILD)/obj/%.o,$(SRCS))
# uACPI (third_party/uacpi, MIT): the ACPI interpreter, for the DSDT's AML
# (PCI interrupt routing). Built with the kernel's flags and its own headers.
UACPI_SRCS := $(wildcard third_party/uacpi/source/*.c)
UACPI_OBJS := $(patsubst third_party/uacpi/source/%.c,$(BUILD)/obj/uacpi/%.o,$(UACPI_SRCS))
UACPI_CFLAGS := -Ithird_party/uacpi/include -DUACPI_SIZED_FREES
CFLAGS += $(UACPI_CFLAGS)
OBJS += $(UACPI_OBJS)

# User programs: libvexa plus one directory per program under userland/.
# -nostdinc keeps the host's C library headers out; only the compiler's own
# (stdint.h, stdarg.h...) and libvexa's are used.
USER_CFLAGS := -g -O2 -pipe -std=gnu11 -Wall -Wextra -Werror \
	-ffreestanding -fno-stack-protector -fPIC -fno-asynchronous-unwind-tables -m64 -march=x86-64 \
	-nostdinc -isystem $(shell $(CC) -print-file-name=include) \
	-Ilibvexa/include -Iabi -MMD -MP
# Programs are position independent and use libvexa.so through the native
# dynamic loader, /lib/vexa-ld.so; STATIC_PROGRAMS link libvexa in instead.
USER_LDFLAGS := -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -z norelro --hash-style=sysv
STATIC_LDFLAGS := $(USER_LDFLAGS) -static --no-dynamic-linker -T libvexa/program.ld
DYNAMIC_LDFLAGS := $(USER_LDFLAGS) -pie -dynamic-linker /lib/vexa-ld.so
STATIC_PROGRAMS := hello-world
# Programs that run as root whoever starts them (they check who may).
SETUID_PROGRAMS := vauth sudo accounts install hostname

LIBVEXA_SRCS := $(wildcard libvexa/src/*.c libvexa/src/*.S)
LIBVEXA_OBJS := $(patsubst libvexa/src/%,$(BUILD)/libvexa/%.o,$(LIBVEXA_SRCS))
CRT0 := $(BUILD)/libvexa/crt0.S.o
DSO_O := $(BUILD)/libvexa-crt/dso.o
# The math library (libm) is musl's, built position independent and linked
# into libvexa: it uses nothing of musl's C library.
MUSL_VERSION := 1.2.4
MUSL_URL := https://archive.ubuntu.com/ubuntu/pool/universe/m/musl/musl_$(MUSL_VERSION).orig.tar.gz
MUSL_SHA256 := 7a35eae33d5372a7c0da1188de798726f68825513b7ae3ebe97aaaa52114f039
MUSL_TARBALL := third_party/musl-$(MUSL_VERSION).tar.gz
LIBM := $(BUILD)/libm/libm.a
LIBVEXA_SO := $(BUILD)/lib/libvexa.so
VEXA_LD := $(BUILD)/lib/vexa-ld.so
VEXA_LD_OBJS := $(patsubst libvexa/ld/%,$(BUILD)/vexa-ld/%.o,$(wildcard libvexa/ld/*.c libvexa/ld/*.S))
PROGRAMS := $(notdir $(wildcard userland/*))
PROGRAM_BINS := $(addprefix $(BUILD)/programs/,$(PROGRAMS))
INITRAMFS := $(BUILD)/initramfs.tar
ROOTFS_FILES := $(shell find rootfs -type f)
APP_FILES := $(shell find apps -type f)

# BusyBox, the first Linux program Vexa runs: built from a pinned release with
# musl (a static binary), and put in /linux/bin. It's GPL-2.0, so
# `make busybox-source` packs the exact source and configuration used, which
# the releases publish next to the ISO.
BUSYBOX_VERSION := 1_36_1
BUSYBOX_REPO := https://github.com/mirror/busybox.git
BUSYBOX_SRC := third_party/busybox
BUSYBOX_BUILD := $(BUILD)/busybox
BUSYBOX := $(BUSYBOX_BUILD)/busybox
BUSYBOX_SOURCE_TARBALL := $(BUILD)/busybox-$(BUSYBOX_VERSION)-source.tar.gz
MUSL_CC ?= musl-gcc
# musl's C library, which is also its dynamic loader (/lib/ld-musl-x86_64.so.1).
MUSL_LIBC ?= /usr/lib/x86_64-linux-musl/libc.so

# GNU bash, from Ubuntu's copy of the upstream release (pinned by checksum).
BASH_VERSION := 5.2.37
BASH_URL := https://archive.ubuntu.com/ubuntu/pool/main/b/bash/bash_$(BASH_VERSION).orig.tar.xz
BASH_SHA256 := 370704c9c859f4060b7df19055e43bb9b5fa09d887699cf6ba87885c5485d36a
BASH_TARBALL := third_party/bash-$(BASH_VERSION).tar.xz
BASH_BUILD := $(BUILD)/bash-$(BASH_VERSION)
BASH := $(BASH_BUILD)/bash

# GNU coreutils: one binary, with a link per command (instead of BusyBox's).
COREUTILS_VERSION := 9.4
COREUTILS_URL := https://archive.ubuntu.com/ubuntu/pool/main/c/coreutils/coreutils_$(COREUTILS_VERSION).orig.tar.xz
COREUTILS_SHA256 := ea613a4cf44612326e917201bbbcdfbd301de21ffc3b59b6e5c07e040b275e52
COREUTILS_TARBALL := third_party/coreutils-$(COREUTILS_VERSION).tar.xz
COREUTILS_BUILD := $(BUILD)/coreutils-$(COREUTILS_VERSION)
COREUTILS := $(COREUTILS_BUILD)/src/coreutils

# zlib (for Python's zlib module), as a static library.
ZLIB_URL := https://archive.ubuntu.com/ubuntu/pool/main/z/zlib/zlib_1.3.dfsg.orig.tar.xz
ZLIB_SHA256 := 5eea0322c1c21c75cad3b607ac1c43ff5c71e014b8ac4a34300b5e2b80d02e70
ZLIB_TARBALL := third_party/zlib-1.3.tar.xz
ZLIB_PREFIX := $(BUILD)/zlib
ZLIB := $(ZLIB_PREFIX)/lib/libz.a

# libffi (for Python's ctypes), as a static library, from the official release.
LIBFFI_VERSION := 3.4.6
LIBFFI_URL := https://github.com/libffi/libffi/releases/download/v$(LIBFFI_VERSION)/libffi-$(LIBFFI_VERSION).tar.gz
LIBFFI_SHA256 := b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e
LIBFFI_TARBALL := third_party/libffi-$(LIBFFI_VERSION).tar.gz
LIBFFI_PREFIX := $(BUILD)/libffi
LIBFFI := $(LIBFFI_PREFIX)/lib/libffi.a

# OpenSSL (TLS for curl and Python's ssl module), as shared libraries, with
# its settings in /etc/ssl (where the root certificates are).
OPENSSL_VERSION := 3.0.13
OPENSSL_URL := https://archive.ubuntu.com/ubuntu/pool/main/o/openssl/openssl_$(OPENSSL_VERSION).orig.tar.gz
OPENSSL_SHA256 := 88525753f79d3bec27d2fa7c66aa0b92b3aa9498dafd93d7cfa4b3780cdae313
OPENSSL_TARBALL := third_party/openssl-$(OPENSSL_VERSION).tar.gz
OPENSSL_BUILD := $(BUILD)/openssl-$(OPENSSL_VERSION)
OPENSSL_ROOT := $(BUILD)/openssl-root
OPENSSL := $(OPENSSL_ROOT)/.done

# curl (HTTPS, through OpenSSL), with libcurl built in.
CURL_VERSION := 8.5.0
CURL_URL := https://archive.ubuntu.com/ubuntu/pool/main/c/curl/curl_$(CURL_VERSION).orig.tar.gz
CURL_SHA256 := 05fc17ff25b793a437a0906e0484b82172a9f4de02be5ed447e0cab8c3475add
CURL_TARBALL := third_party/curl-$(CURL_VERSION).tar.gz
CURL_BUILD := $(BUILD)/curl-$(CURL_VERSION)
CURL_ROOT := $(BUILD)/curl-root
CURL := $(CURL_ROOT)/usr/bin/curl

# ALSA's library and aplay/speaker-test, for sound in Linux programs (over
# the kernel's ALSA interface, personality/linux/sound.c).
ALSA_LIB_VERSION := 1.2.11
ALSA_LIB_URL := https://archive.ubuntu.com/ubuntu/pool/main/a/alsa-lib/alsa-lib_$(ALSA_LIB_VERSION).orig.tar.bz2
ALSA_LIB_SHA256 := 9f3f2f69b995f9ad37359072fbc69a3a88bfba081fc83e9be30e14662795bb4d
ALSA_LIB_TARBALL := third_party/alsa-lib-$(ALSA_LIB_VERSION).tar.bz2
ALSA_UTILS_VERSION := 1.2.9
ALSA_UTILS_URL := https://archive.ubuntu.com/ubuntu/pool/main/a/alsa-utils/alsa-utils_$(ALSA_UTILS_VERSION).orig.tar.bz2
ALSA_UTILS_SHA256 := e7623d4525595f92e11ce25ee9a97f2040a14c6e4dcd027aa96e06cbce7817bd
ALSA_UTILS_TARBALL := third_party/alsa-utils-$(ALSA_UTILS_VERSION).tar.bz2
ALSA_ROOT := $(BUILD)/alsa-root
ALSA := $(ALSA_ROOT)/.done

# Python, installed into its own root and pruned (tools/prune-python.sh).
PYTHON_VERSION := 3.12.3
PYTHON_URL := https://archive.ubuntu.com/ubuntu/pool/main/p/python3.12/python3.12_$(PYTHON_VERSION).orig.tar.xz
PYTHON_SHA256 := 56bfef1fdfc1221ce6720e43a661e3eb41785dd914ce99698d8c7896af4bdaa1
PYTHON_TARBALL := third_party/Python-$(PYTHON_VERSION).tar.xz
PYTHON_BUILD := $(BUILD)/Python-$(PYTHON_VERSION)
PYTHON_ROOT := $(BUILD)/python-root
PYTHON := $(PYTHON_ROOT)/.done

# X: the libraries, Xvexa (an X server in a Vexa desktop window), xkbcomp and
# the keyboard data, and xterm, all built with musl (tools/build-x11.sh).
X11_SYSROOT := $(BUILD)/x11
X11 := $(X11_SYSROOT)/.done
X11_SOURCES := third_party/x11-sources.txt
XCLIPBOARD := $(BUILD)/xclipboard/xclipboard

# OpenGL: Mesa with llvmpipe, on LLVM and LLVM's C++ runtime for musl, all
# built from source by tools/build-mesa.sh.
LLVM_VERSION := 18.1.8
LLVM_URL := https://github.com/llvm/llvm-project/releases/download/llvmorg-$(LLVM_VERSION)/llvm-project-$(LLVM_VERSION).src.tar.xz
LLVM_SHA256 := 0b58557a6d32ceee97c8d533a59b9212d87e0fc4d2833924eb6c611247db2f2a
LLVM_TARBALL := third_party/llvm-project-$(LLVM_VERSION).src.tar.xz
MESA_VERSION := 24.0.5
MESA_URL := https://archive.ubuntu.com/ubuntu/pool/main/m/mesa/mesa_$(MESA_VERSION).orig.tar.gz
MESA_SHA256 := 5fd81faf83923fbd5c860c77f9709f82c8e9bb8b91cb4c972ce8b64ec1006b67
MESA_TARBALL := third_party/mesa-$(MESA_VERSION).tar.gz
MESA_ROOT := $(BUILD)/mesa-root
MESA := $(MESA_ROOT)/.done
GL_TEST := $(BUILD)/gl-test/gl-test

# Everything under /linux: the Linux programs and what they need.
LINUX_ROOT := $(BUILD)/linux-root
ifeq ($(LINUX_COMPAT),1)
LINUX_TREE := $(LINUX_ROOT)/.done
endif

# Test disks for `make test`: the same file system (tests/disk-content plus
# hello-world) on three kinds of disk, each with a different layout; ext2,
# but ext3 (with a journal) on the NVMe one.
DISK_CONTENT_FILES := $(shell find tests/disk-content -type f)
TEST_DISKS := $(BUILD)/disks/virtio-gpt.img $(BUILD)/disks/sata-mbr.img \
	$(BUILD)/disks/nvme-whole.img $(BUILD)/disks/ext4.img $(BUILD)/disks/fat32.img \
	$(BUILD)/disks/exfat.img
# A disk for `make run-disk`, created once and kept, so changes survive reboots.
MY_DISK := $(BUILD)/my-disk.img
USER_OBJS := $(LIBVEXA_OBJS) \
	$(patsubst %,$(BUILD)/%.o,$(wildcard $(addsuffix /*.c,$(addprefix userland/,$(PROGRAMS)))))

.PHONY: all openssl curl mesa alsa linux-tarballs kernel programs iso run run-disk run-nographic test test-install test-disks clean distclean \
	busybox busybox-source doom doom-source netsurf netsurf-source bash coreutils python x11 test-native test-quick native-iso \
	test-bios test-uefi test-safe test-native-boot test-virgl run-virgl sdk sdk-test packages

all: iso
kernel: $(KERNEL)
programs: $(PROGRAM_BINS) $(LIBVEXA_SO) $(VEXA_LD)
iso: $(ISO)

$(BUILD)/obj/%.c.o: kernel/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.S.o: kernel/src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/uacpi/%.o: third_party/uacpi/source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/libvexa/%.o: libvexa/src/%
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# Limine's own tool, built for Vexa (the installer runs it), needs Limine first.
$(BUILD)/userland/limine/main.c.o: | limine/limine

.SECONDEXPANSION:
$(BUILD)/userland/%.c.o: userland/%.c $$(PROGRAM_LIBS_$$(firstword $$(subst /, ,$$*)))
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(PROGRAM_CFLAGS_$(firstword $(subst /, ,$*))) -c $< -o $@

# libvexa.so: libvexa without crt0 (which each program carries), binding its
# own references to itself.
$(LIBVEXA_SO): $(filter-out $(CRT0),$(LIBVEXA_OBJS)) $(LIBM)
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -shared -Bsymbolic -soname libvexa.so \
		$(filter %.o,$^) --whole-archive $(LIBM) --no-whole-archive -o $@

# Mbed TLS (TLS for native programs: fetch's HTTPS), built against libvexa;
# in the SDK too.
MBEDTLS_VERSION := 3.6.2
MBEDTLS_URL := https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBEDTLS_VERSION)/mbedtls-$(MBEDTLS_VERSION).tar.bz2
MBEDTLS_SHA256 := 8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
MBEDTLS_TARBALL := third_party/mbedtls-$(MBEDTLS_VERSION).tar.bz2
MBEDTLS_PREFIX := $(BUILD)/mbedtls
MBEDTLS_LIBS := $(MBEDTLS_PREFIX)/lib/libmbedtls.a $(MBEDTLS_PREFIX)/lib/libmbedx509.a \
	$(MBEDTLS_PREFIX)/lib/libmbedcrypto.a

$(MBEDTLS_TARBALL):
	$(call fetch,$(MBEDTLS_URL),$(MBEDTLS_SHA256))

$(MBEDTLS_LIBS) &: $(MBEDTLS_TARBALL) tools/build-mbedtls.sh $(shell find libvexa/include -type f)
	tools/build-mbedtls.sh $(MBEDTLS_TARBALL) $(BUILD)/mbedtls-work $(MBEDTLS_PREFIX) $(CC) \
		$(filter-out -Werror -Wall -Wextra -MMD -MP -Ilibvexa/include -Iabi,$(USER_CFLAGS)) \
		-I$(abspath libvexa/include) -I$(abspath abi) -D__vexa__=1

# Programs that link more than libvexa, and what they compile with.
PROGRAM_LIBS_fetch := $(MBEDTLS_LIBS) $(shell $(CC) -print-libgcc-file-name)
PROGRAM_CFLAGS_fetch := -I$(MBEDTLS_PREFIX)/include

# zlib's compression core, built against libvexa, for archive (zip, tar.gz).
ZLIB_NATIVE_DIR := $(BUILD)/zlib-native/zlib-1.3.dfsg
ZLIB_NATIVE_PARTS := adler32 crc32 deflate inflate inftrees inffast trees zutil
ZLIB_NATIVE := $(BUILD)/zlib-native/libz.a
$(ZLIB_NATIVE): $(ZLIB_TARBALL)
	rm -rf $(BUILD)/zlib-native && mkdir -p $(BUILD)/zlib-native
	tar -xJf $(ZLIB_TARBALL) -C $(BUILD)/zlib-native
	for part in $(ZLIB_NATIVE_PARTS); do \
		$(CC) $(filter-out -Werror -Wall -Wextra -MMD -MP -Ilibvexa/include -Iabi,$(USER_CFLAGS)) \
			-I$(abspath libvexa/include) -I$(abspath abi) \
			-c $(ZLIB_NATIVE_DIR)/$$part.c -o $(ZLIB_NATIVE_DIR)/$$part.o || exit 1; \
	done
	ar rcs $@ $(addprefix $(ZLIB_NATIVE_DIR)/,$(addsuffix .o,$(ZLIB_NATIVE_PARTS)))
PROGRAM_LIBS_archive := $(ZLIB_NATIVE)
PROGRAM_CFLAGS_archive := -I$(ZLIB_NATIVE_DIR)

# A shared library for posix-test's dlopen checks, in /lib.
DLTEST_SO := $(BUILD)/lib/libvexa-test.so
$(DLTEST_SO): tests/dltest/dltest.c $(LIBVEXA_SO)
	@mkdir -p $(BUILD)/dltest
	$(CC) $(USER_CFLAGS) -c $< -o $(BUILD)/dltest/dltest.o
	$(LD) $(USER_LDFLAGS) -shared -soname libvexa-test.so $(BUILD)/dltest/dltest.o $(LIBVEXA_SO) -o $@

$(MUSL_TARBALL):
	$(call fetch,$(MUSL_URL),$(MUSL_SHA256))

$(LIBM): $(MUSL_TARBALL) tools/build-libm.sh
	tools/build-libm.sh $(MUSL_TARBALL) $(BUILD)/libm-work $@

# The dynamic loader relocates itself before it uses any pointer, so all its
# symbols are hidden: it needs only relative relocations.
$(BUILD)/vexa-ld/%.o: libvexa/ld/%
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -fvisibility=hidden -c $< -o $@

$(VEXA_LD): $(VEXA_LD_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -shared -Bsymbolic -e _start $^ -o $@

$(BUILD)/programs/%: $(CRT0) $(LIBVEXA_SO) $(LIBVEXA_OBJS) $(LIBM) libvexa/program.ld \
		$$(addprefix $(BUILD)/,$$(addsuffix .o,$$(wildcard userland/$$*/*.c))) $$(PROGRAM_LIBS_$$*)
	@mkdir -p $(dir $@)
	$(if $(filter $*,$(STATIC_PROGRAMS)), \
		$(LD) $(STATIC_LDFLAGS) $(sort $(filter %.o,$^)) $(PROGRAM_LIBS_$*) $(LIBM) -o $@, \
		$(LD) $(DYNAMIC_LDFLAGS) $(CRT0) $(filter-out $(LIBVEXA_OBJS),$(filter %.o,$^)) \
			$(PROGRAM_LIBS_$*) $(LIBVEXA_SO) -o $@)

# ---- The SDK: building native apps on another machine ----
#
# build/sdk/vexa-sdk: libvexa's headers and libraries (libvexa.so for the
# loader, libvexa.a for static programs, crt0.o), vexa-cc (the build
# machine's compiler, set up for Vexa), vexa-new-app and the app template, a
# CMake toolchain file, and the guide (docs/SDK.md). The releases publish it
# as vexa-sdk-<version>.tar.gz.
VEXA_VERSION := $(shell sed -n 's/^\#define VEXA_VERSION "\(.*\)"/\1/p' kernel/include/vexa/version.h)
SDK_DIR := $(BUILD)/sdk/vexa-sdk
SDK_TARBALL := $(BUILD)/vexa-sdk-$(VEXA_VERSION).tar.gz
SDK_FILES := $(shell find sdk -type f -not -path '*/build/*') docs/SDK.md
LIBVEXA_A := $(BUILD)/lib/libvexa.a

$(LIBVEXA_A): $(filter-out $(CRT0),$(LIBVEXA_OBJS)) $(LIBM)
	@mkdir -p $(dir $@)
	rm -f $@
	ar rcs $@ $(filter %.o,$^)
	printf 'OPEN %s\nADDLIB %s\nSAVE\nEND\n' $@ $(LIBM) | ar -M

$(DSO_O): libvexa/crt/dso.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(SDK_DIR)/.done: $(SDK_FILES) $(LIBVEXA_SO) $(LIBVEXA_A) $(CRT0) $(DSO_O) libvexa/program.ld \
		$(shell find libvexa/include -type f) abi/vexa/abi.h $(MUSL_TARBALL)
	rm -rf $(SDK_DIR)
	mkdir -p $(SDK_DIR)/include/vexa $(SDK_DIR)/lib $(SDK_DIR)/licenses
	cp -R sdk/. $(SDK_DIR)/
	rm -rf $(SDK_DIR)/template/build $(SDK_DIR)/examples/*/build
	cp docs/SDK.md $(SDK_DIR)/README.md
	cp -R libvexa/include/. $(SDK_DIR)/include/
	cp abi/vexa/abi.h $(SDK_DIR)/include/vexa/
	cp $(LIBVEXA_SO) $(LIBVEXA_A) libvexa/program.ld $(SDK_DIR)/lib/
	cp $(CRT0) $(SDK_DIR)/lib/crt0.o
	cp $(DSO_O) $(SDK_DIR)/lib/dso.o
	# -lm, -lpthread...: all in libvexa, so these are empty.
	for lib in c m pthread rt dl iconv; do ar rc $(SDK_DIR)/lib/lib$$lib.a; done
	tar -xzf $(MUSL_TARBALL) -O musl-$(MUSL_VERSION)/COPYRIGHT > $(SDK_DIR)/licenses/musl-libm.txt
	echo "$(VEXA_VERSION)" > $(SDK_DIR)/VERSION
	touch $@

# SDL 2, built with the SDK (only again when its sources, Vexa's drivers or
# libvexa's headers change) and added to it.
SDL2_VERSION := 2.30.12
SDL2_URL := https://github.com/libsdl-org/SDL/releases/download/release-$(SDL2_VERSION)/SDL2-$(SDL2_VERSION).tar.gz
SDL2_SHA256 := ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea
SDL2_TARBALL := third_party/SDL2-$(SDL2_VERSION).tar.gz
SDL2_PREFIX := $(BUILD)/sdl2
SDL2 := $(SDL2_PREFIX)/lib/libSDL2.a

$(SDL2_TARBALL):
	$(call fetch,$(SDL2_URL),$(SDL2_SHA256))

$(SDL2): $(SDL2_TARBALL) tools/build-sdl2.sh $(shell find sdk/sdl2 -type f) sdk/bin/vexa-cc \
		$(shell find libvexa/include -type f) | $(SDK_DIR)/.done
	tools/build-sdl2.sh $(SDL2_TARBALL) $(BUILD)/sdl2-work $(SDK_DIR) $(SDL2_PREFIX)

# SDL_mixer 2 (sound effects and music for SDL programs), likewise.
SDL2_MIXER_VERSION := 2.8.0
SDL2_MIXER_URL := https://github.com/libsdl-org/SDL_mixer/releases/download/release-$(SDL2_MIXER_VERSION)/SDL2_mixer-$(SDL2_MIXER_VERSION).tar.gz
SDL2_MIXER_SHA256 := 1cfb34c87b26dbdbc7afd68c4f545c0116ab5f90bbfecc5aebe2a9cb4bb31549
SDL2_MIXER_TARBALL := third_party/SDL2_mixer-$(SDL2_MIXER_VERSION).tar.gz
SDL2_MIXER_PREFIX := $(BUILD)/sdl2-mixer
SDL2_MIXER := $(SDL2_MIXER_PREFIX)/lib/libSDL2_mixer.a

$(SDL2_MIXER_TARBALL):
	$(call fetch,$(SDL2_MIXER_URL),$(SDL2_MIXER_SHA256))

$(SDL2_MIXER): $(SDL2_MIXER_TARBALL) tools/build-sdl2-mixer.sh $(SDL2)
	tools/build-sdl2-mixer.sh $(SDL2_MIXER_TARBALL) $(BUILD)/sdl2-mixer-work $(SDK_DIR) \
		$(SDL2_PREFIX) $(SDL2_MIXER_PREFIX)

# SDL_net 2 (TCP and UDP, over libvexa's sockets), likewise.
SDL2_NET_VERSION := 2.2.0
SDL2_NET_URL := https://github.com/libsdl-org/SDL_net/releases/download/release-$(SDL2_NET_VERSION)/SDL2_net-$(SDL2_NET_VERSION).tar.gz
SDL2_NET_SHA256 := 4e4a891988316271974ff4e9585ed1ef729a123d22c08bd473129179dc857feb
SDL2_NET_TARBALL := third_party/SDL2_net-$(SDL2_NET_VERSION).tar.gz
SDL2_NET_PREFIX := $(BUILD)/sdl2-net
SDL2_NET := $(SDL2_NET_PREFIX)/lib/libSDL2_net.a

$(SDL2_NET_TARBALL):
	$(call fetch,$(SDL2_NET_URL),$(SDL2_NET_SHA256))

$(SDL2_NET): $(SDL2_NET_TARBALL) tools/build-sdl2-net.sh $(SDL2) $(shell find libvexa/include -type f)
	tools/build-sdl2-net.sh $(SDL2_NET_TARBALL) $(BUILD)/sdl2-net-work $(SDK_DIR) \
		$(SDL2_PREFIX) $(SDL2_NET_PREFIX)

# C++: LLVM's libc++ and libc++abi, built against libvexa (no exceptions, no
# locales), for vexa-c++.
LIBCXX_VEXA_DIR := $(BUILD)/libcxx-vexa
LIBCXX_VEXA := $(LIBCXX_VEXA_DIR)/lib/libc++.a
$(LIBCXX_VEXA): $(LLVM_TARBALL) tools/build-libcxx-vexa.sh $(SDK_DIR)/.done
	rm -rf $(LIBCXX_VEXA_DIR)
	tools/build-libcxx-vexa.sh $(LLVM_TARBALL) $(SDK_DIR) $(BUILD)/libcxx-vexa-work $(LIBCXX_VEXA_DIR)

$(SDK_DIR)/.cxx: $(SDK_DIR)/.done $(LIBCXX_VEXA)
	mkdir -p $(SDK_DIR)/include/c++
	cp -R $(LIBCXX_VEXA_DIR)/usr/include/c++/v1 $(SDK_DIR)/include/c++/
	cp $(LIBCXX_VEXA_DIR)/lib/libc++.a $(LIBCXX_VEXA_DIR)/lib/libc++abi.a $(SDK_DIR)/lib/
	tar -xJf $(LLVM_TARBALL) -O llvm-project-$(LLVM_VERSION).src/libcxx/LICENSE.TXT \
		> $(SDK_DIR)/licenses/libcxx.txt
	touch $@

# OpenGL for native programs: Mesa's OSMesa with softpipe, built with the
# SDK (C and C++) as /lib/libOSMesa.so (on the boot CD, /cdrom/lib, linked
# from /lib), with its headers (GL/, KHR/) in the SDK. SDL's Vexa driver
# loads it for SDL_GL_*.
MESA_VEXA_DIR := $(BUILD)/mesa-vexa
MESA_VEXA := $(MESA_VEXA_DIR)/lib/libOSMesa.so
MESA_VEXA_LIB ?= $(MESA_VEXA)
$(MESA_VEXA): $(MESA_TARBALL) tools/build-mesa-vexa.sh third_party/mesa-vexa.patch \
		$(wildcard third_party/libdrm-vexa/*) $(SDK_DIR)/.cxx
	tools/build-mesa-vexa.sh $(MESA_TARBALL) $(SDK_DIR) $(BUILD)/mesa-vexa-work $(MESA_VEXA_DIR)

$(SDK_DIR)/.complete: $(SDK_DIR)/.cxx $(SDL2) $(SDL2_MIXER) $(SDL2_NET) $(MBEDTLS_LIBS) $(MESA_VEXA)
	cp -R $(MESA_VEXA_DIR)/include/. $(SDK_DIR)/include/
	cp $(MESA_VEXA) $(SDK_DIR)/lib/
	cp $(MESA_VEXA_DIR)/license.rst $(SDK_DIR)/licenses/mesa.txt
	cp -R $(SDL2_PREFIX)/. $(SDK_DIR)/
	cp -R $(SDL2_MIXER_PREFIX)/. $(SDK_DIR)/
	cp -R $(SDL2_NET_PREFIX)/. $(SDK_DIR)/
	cp -R $(MBEDTLS_PREFIX)/. $(SDK_DIR)/
	touch $@

$(SDK_TARBALL): $(SDK_DIR)/.complete
	tar -C $(dir $(SDK_DIR)) --owner=0 --group=0 --numeric-owner --sort=name \
		--exclude=.done --exclude=.cxx --exclude=.complete --transform 's,^vexa-sdk,vexa-sdk-$(VEXA_VERSION),' \
		-czf $@ vexa-sdk

sdk: $(SDK_TARBALL)

# The SDK's own check: a new app from the template and the SDL demo (built
# here and put on the test disks, where the smoke test opens them), and a
# CMake build with SDL.
SDK_TEST := $(BUILD)/sdk-test
SDK_HELLO := $(SDK_TEST)/hello-app/build/HelloSDK.vxapp/.done
SDK_SDL_DEMO := $(SDK_TEST)/sdl-demo/build/SDLDemo.vxapp
$(SDK_HELLO): $(SDK_DIR)/.complete
	rm -rf $(SDK_TEST)
	mkdir -p $(SDK_TEST)
	cd $(SDK_TEST) && $(abspath $(SDK_DIR))/bin/vexa-new-app "Hello SDK" hello-app
	$(MAKE) -C $(SDK_TEST)/hello-app
	cp -R $(SDK_DIR)/examples/sdl-demo $(SDK_TEST)/sdl-demo
	$(MAKE) -C $(SDK_TEST)/sdl-demo SDK=$(abspath $(SDK_DIR))
	printf 'cmake_minimum_required(VERSION 3.13)\nproject(t C)\nfind_package(SDL2 REQUIRED)\nadd_executable(t t.c)\ntarget_link_libraries(t SDL2::SDL2 m pthread)\n' \
		> $(SDK_TEST)/CMakeLists.txt
	printf '#include <SDL.h>\n#include <math.h>\nint main(void) { SDL_Log("%%g", sqrt(2.0)); return SDL_Init(0); }\n' \
		> $(SDK_TEST)/t.c
	cmake -S $(SDK_TEST) -B $(SDK_TEST)/cmake -DCMAKE_TOOLCHAIN_FILE=$(abspath $(SDK_DIR))/cmake/vexa.cmake >/dev/null
	cmake --build $(SDK_TEST)/cmake >/dev/null
	readelf -l $(SDK_TEST)/cmake/t | grep -q 'vexa-ld.so'
	touch $@

sdk-test: $(SDK_HELLO)

# pkg's packages (for the "packages" release, which pkg and Software read):
# the SDK's example apps, zipped, with index.conf.
PACKAGES := $(BUILD)/packages/index.conf
$(PACKAGES): $(SDK_HELLO) tools/make-packages.py
	tools/make-packages.py $(BUILD)/packages \
		"hello:1.0:Accessories:A window that says hello (the SDK's app template):$(SDK_TEST)/hello-app/build/HelloSDK.vxapp" \
		"sdl-demo:1.0:Games:SDL 2 on Vexa - pictures, a chime, and keys:$(SDK_SDL_DEMO)"

packages: $(PACKAGES)

# cxx-test: a C++ program built with the SDK's vexa-c++ (libc++ on libvexa).
CXX_TEST := $(BUILD)/cxx-test/cxx-test
CXX_TEST_STATIC := $(BUILD)/cxx-test/cxx-test-static
CXX_TEST_PROGRAM ?= $(CXX_TEST) $(CXX_TEST_STATIC)
$(CXX_TEST): tests/cxx/cxx-test.cpp $(SDK_DIR)/.complete
	@mkdir -p $(dir $@)
	$(SDK_DIR)/bin/vexa-c++ -O2 -std=c++17 -Wall $< -o $@
$(CXX_TEST_STATIC): tests/cxx/cxx-test.cpp $(SDK_DIR)/.complete
	@mkdir -p $(dir $@)
	$(SDK_DIR)/bin/vexa-c++ -static -O2 -std=c++17 -Wall $< -o $@

# sdl-gl-test: OpenGL in a native program, through SDL and Mesa (built with
# the SDK, linked to libOSMesa.so for its gl* calls).
SDL_GL_TEST := $(BUILD)/sdl-gl-test/sdl-gl-test
SDL_GL_TEST_PROGRAM ?= $(SDL_GL_TEST)
$(SDL_GL_TEST): tests/gl/sdl-gl-test.c $(SDK_DIR)/.complete
	@mkdir -p $(dir $@)
	$(SDK_DIR)/bin/vexa-cc -O2 -Wall -Wextra $$($(SDK_DIR)/bin/sdl2-config --cflags) $< -o $@ \
		$$($(SDK_DIR)/bin/sdl2-config --libs) -lOSMesa

# NetSurf (GPL-2.0), the web browser: built with the SDK as a native app,
# drawing into a desktop window through libnsfb's Vexa surface. Its own
# libraries come in its source bundle (here from Ubuntu's archive); zlib,
# libpng, libjpeg-turbo, FreeType, expat and curl (on the SDK's Mbed TLS) are
# built first, as static libraries.
NETSURF_VERSION := 3.11
NETSURF_URL := https://archive.ubuntu.com/ubuntu/pool/universe/n/netsurf/netsurf_$(NETSURF_VERSION).orig.tar.gz
NETSURF_SHA256 := 4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b
NETSURF_TARBALL := third_party/netsurf_$(NETSURF_VERSION).orig.tar.gz
JPEG_URL := https://archive.ubuntu.com/ubuntu/pool/main/libj/libjpeg-turbo/libjpeg-turbo_2.1.5.orig.tar.gz
JPEG_SHA256 := 254f3642b04e309fee775123133c6464181addc150499561020312ec61c1bf7c
JPEG_TARBALL := third_party/libjpeg-turbo_2.1.5.orig.tar.gz
PNG_URL := https://archive.ubuntu.com/ubuntu/pool/main/libp/libpng1.6/libpng1.6_1.6.43.orig.tar.gz
PNG_SHA256 := fecc95b46cf05e8e3fc8a414750e0ba5aad00d89e9fdf175e94ff041caf1a03a
PNG_TARBALL := third_party/libpng1.6_1.6.43.orig.tar.gz
FREETYPE_URL := https://archive.ubuntu.com/ubuntu/pool/main/f/freetype/freetype_2.13.2+dfsg.orig.tar.xz
FREETYPE_SHA256 := 48c78a4194adfcd15a4d089f3206dab8454c311f5577f3ef7eaef95f777f86e6
FREETYPE_TARBALL := third_party/freetype_2.13.2+dfsg.orig.tar.xz
EXPAT_URL := https://github.com/libexpat/libexpat/releases/download/R_2_6_1/expat-2.6.1.tar.xz
EXPAT_SHA256 := 0c00d2760ad12efef6e26efc8b363c8eb28eb8c8de719e46d5bb67b40ba904a3
EXPAT_TARBALL := third_party/expat-2.6.1.tar.xz
NETSURF_DEPS := $(BUILD)/netsurf-deps/lib/libcurl.a
NETSURF_DIR := $(BUILD)/netsurf
NETSURF := $(NETSURF_DIR)/netsurf
NETSURF_PROGRAM ?= $(NETSURF)
NETSURF_RES ?= $(NETSURF_DIR)/res

$(NETSURF_TARBALL):
	$(call fetch,$(NETSURF_URL),$(NETSURF_SHA256))
$(JPEG_TARBALL):
	$(call fetch,$(JPEG_URL),$(JPEG_SHA256))
$(PNG_TARBALL):
	$(call fetch,$(PNG_URL),$(PNG_SHA256))
$(FREETYPE_TARBALL):
	$(call fetch,$(FREETYPE_URL),$(FREETYPE_SHA256))
$(EXPAT_TARBALL):
	$(call fetch,$(EXPAT_URL),$(EXPAT_SHA256))

$(NETSURF_DEPS): tools/build-netsurf-deps.sh $(SDK_DIR)/.complete $(ZLIB_TARBALL) $(PNG_TARBALL) \
		$(JPEG_TARBALL) $(FREETYPE_TARBALL) $(CURL_TARBALL) $(EXPAT_TARBALL)
	rm -rf $(BUILD)/netsurf-deps $(BUILD)/netsurf-deps-work
	tools/build-netsurf-deps.sh $(SDK_DIR) $(BUILD)/netsurf-deps-work $(BUILD)/netsurf-deps \
		$(ZLIB_TARBALL) $(PNG_TARBALL) $(JPEG_TARBALL) $(FREETYPE_TARBALL) $(CURL_TARBALL) \
		$(EXPAT_TARBALL)

$(NETSURF): $(NETSURF_TARBALL) tools/build-netsurf.sh third_party/netsurf-vexa.patch $(NETSURF_DEPS)
	tools/build-netsurf.sh $(NETSURF_TARBALL) $(SDK_DIR) $(BUILD)/netsurf-deps $(BUILD)/netsurf-build \
		$(NETSURF_DIR)

netsurf: $(NETSURF)

# NetSurf's source (GPL-2.0) and Vexa's patch, which the releases publish.
NETSURF_SOURCE_TARBALL := $(BUILD)/netsurf-$(NETSURF_VERSION)-source.tar.gz
$(NETSURF_SOURCE_TARBALL): $(NETSURF_TARBALL) third_party/netsurf-vexa.patch tools/build-netsurf.sh \
		tools/build-netsurf-deps.sh
	rm -rf $(BUILD)/netsurf-source
	mkdir -p $(BUILD)/netsurf-source/netsurf-$(NETSURF_VERSION)
	cp $(NETSURF_TARBALL) third_party/netsurf-vexa.patch tools/build-netsurf.sh \
		tools/build-netsurf-deps.sh $(BUILD)/netsurf-source/netsurf-$(NETSURF_VERSION)/
	tar -C $(BUILD)/netsurf-source --owner=0 --group=0 --numeric-owner --sort=name --mtime=@0 \
		-czf $@ netsurf-$(NETSURF_VERSION)

netsurf-source: $(NETSURF_SOURCE_TARBALL)

# Doom: Chocolate Doom (GPL-2.0), built with the SDK like any SDL program,
# with Freedoom's levels and art (BSD), which stay on the boot CD (/cdrom/doom)
# until they're played. `make doom-source` packs its source and Vexa's patch,
# which the releases publish next to the ISO.
CHOCOLATE_DOOM_VERSION := 3.1.0
CHOCOLATE_DOOM_REPO := https://github.com/chocolate-doom/chocolate-doom
CHOCOLATE_DOOM_COMMIT := 35fb1372d10756ca27eca05665bd8a7cebc71c05
CHOCOLATE_DOOM_SRC := third_party/chocolate-doom-$(CHOCOLATE_DOOM_VERSION)
CHOCOLATE_DOOM_SOURCE_TARBALL := $(BUILD)/chocolate-doom-$(CHOCOLATE_DOOM_VERSION)-source.tar.gz
DOOM := $(BUILD)/doom/chocolate-doom
# (The native ISO's build uses this one.)
DOOM_PROGRAM ?= $(DOOM)
FREEDOOM_VERSION := 0.13.0
FREEDOOM_URL := https://github.com/freedoom/freedoom/releases/download/v$(FREEDOOM_VERSION)/freedoom-$(FREEDOOM_VERSION).zip
FREEDOOM_SHA256 := 3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
FREEDOOM_ZIP := third_party/freedoom-$(FREEDOOM_VERSION).zip
FREEDOOM_DIR ?= $(BUILD)/freedoom

$(CHOCOLATE_DOOM_SRC)/CMakeLists.txt:
	rm -rf $(CHOCOLATE_DOOM_SRC)
	git clone -q --depth 1 --branch chocolate-doom-$(CHOCOLATE_DOOM_VERSION) \
		$(CHOCOLATE_DOOM_REPO) $(CHOCOLATE_DOOM_SRC)
	test "$$(git -C $(CHOCOLATE_DOOM_SRC) rev-parse HEAD)" = $(CHOCOLATE_DOOM_COMMIT)

$(DOOM): $(CHOCOLATE_DOOM_SRC)/CMakeLists.txt ports/chocolate-doom/vexa.patch tools/build-doom.sh \
		$(SDK_DIR)/.complete
	tools/build-doom.sh $(CHOCOLATE_DOOM_SRC) ports/chocolate-doom/vexa.patch $(BUILD)/doom-work \
		$(SDK_DIR) $@

$(FREEDOOM_ZIP):
	$(call fetch,$(FREEDOOM_URL),$(FREEDOOM_SHA256))

$(BUILD)/freedoom/freedoom1.wad: $(FREEDOOM_ZIP)
	rm -rf $(BUILD)/freedoom && mkdir -p $(BUILD)/freedoom
	unzip -q -j $(FREEDOOM_ZIP) 'freedoom-$(FREEDOOM_VERSION)/freedoom1.wad' \
		'freedoom-$(FREEDOOM_VERSION)/COPYING.txt' 'freedoom-$(FREEDOOM_VERSION)/CREDITS.txt' \
		-d $(BUILD)/freedoom
	touch $@

$(CHOCOLATE_DOOM_SOURCE_TARBALL): $(CHOCOLATE_DOOM_SRC)/CMakeLists.txt ports/chocolate-doom/vexa.patch
	git -C $(CHOCOLATE_DOOM_SRC) archive --format=tar \
		--prefix=chocolate-doom-$(CHOCOLATE_DOOM_VERSION)/ HEAD > $(BUILD)/doom-source.tar
	tar --append -f $(BUILD)/doom-source.tar \
		--transform 's,^ports/chocolate-doom/,chocolate-doom-$(CHOCOLATE_DOOM_VERSION)/vexa-,' \
		ports/chocolate-doom/vexa.patch
	gzip -9n < $(BUILD)/doom-source.tar > $@
	rm $(BUILD)/doom-source.tar

doom: $(DOOM) $(BUILD)/freedoom/freedoom1.wad
doom-source: $(CHOCOLATE_DOOM_SOURCE_TARBALL)

# /linux is split: what's written to stays in memory, the rest is read from
# the boot CD when it's used (the ISO's /linux; /cdrom is the boot CD).
LINUX_IN_MEMORY := etc var root
LINUX_ON_CD := bin lib sbin usr

# The starting root file system: rootfs/ plus the programs in /bin and the
# apps in /apps, as a tar archive (ustar, with fixed owners and times so
# builds are reproducible).
$(INITRAMFS): $(PROGRAM_BINS) $(LIBVEXA_SO) $(VEXA_LD) $(DLTEST_SO) $(ROOTFS_FILES) $(APP_FILES) $(LINUX_TREE) \
		docs/USER-GUIDE.md $(DOOM_PROGRAM) $(CXX_TEST_PROGRAM) $(SDL_GL_TEST_PROGRAM) \
		$(NETSURF_PROGRAM)
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin $(BUILD)/rootfs/lib
	cp -R rootfs/. $(BUILD)/rootfs/
	# The Help app's book: the user guide.
	mkdir -p $(BUILD)/rootfs/share/help
	cp docs/USER-GUIDE.md $(BUILD)/rootfs/share/help/
	cp $(PROGRAM_BINS) $(DOOM_PROGRAM) $(CXX_TEST_PROGRAM) $(SDL_GL_TEST_PROGRAM) $(NETSURF_PROGRAM) \
		$(BUILD)/rootfs/bin/
	cp $(LIBVEXA_SO) $(VEXA_LD) $(DLTEST_SO) $(BUILD)/rootfs/lib/
	# The standard root certificates, for native programs (fetch's HTTPS).
	mkdir -p $(BUILD)/rootfs/etc/ssl/certs
	cp tools/linux-files/ca-certificates.crt $(BUILD)/rootfs/etc/ssl/certs/
	# Apps (.vxapp bundles): each app's program moves into its bundle, and
	# /bin keeps a link to it for the command line.
	cp -R apps $(BUILD)/rootfs/apps
	for bundle in $(BUILD)/rootfs/apps/*.vxapp; do \
		exe=$$(sed -n 's/^executable=//p' $$bundle/Contents/Info.conf); \
		case $$exe in /*) continue;; esac; \
		mkdir -p $$bundle/Contents/Vexa && \
		mv $(BUILD)/rootfs/bin/$$exe $$bundle/Contents/Vexa/ && \
		ln -s /apps/$${bundle##*/}/Contents/Vexa/$$exe $(BUILD)/rootfs/bin/$$exe || exit 1; \
	done
	# Doom's game files are on the boot CD (make_iso puts them there).
	ln -s /cdrom/doom/freedoom1.wad $(BUILD)/rootfs/apps/Doom.vxapp/Contents/Resources/freedoom1.wad
	# NetSurf's resources (its pages, style sheets, messages, pictures).
	cp -R $(NETSURF_RES)/. $(BUILD)/rootfs/apps/NetSurf.vxapp/Contents/Resources/
	# OpenGL (Mesa) is on the boot CD too.
	ln -s /cdrom/lib/libOSMesa.so $(BUILD)/rootfs/lib/libOSMesa.so
	# /linux: the files that change (etc, var, root) are here, in memory;
	# the programs and libraries stay on the boot CD (see make_iso).
	if [ -n "$(LINUX_TREE)" ]; then \
		mkdir -p $(BUILD)/rootfs/linux && \
		for dir in $(LINUX_IN_MEMORY); do cp -a $(LINUX_ROOT)/$$dir $(BUILD)/rootfs/linux/; done && \
		for dir in $(LINUX_ON_CD); do ln -s /cdrom/linux/$$dir $(BUILD)/rootfs/linux/$$dir; done && \
		for file in passwd group shadow; do ln -sf /etc/$$file $(BUILD)/rootfs/linux/etc/$$file; done; fi
	# Accounts: the password hashes are root's alone, and these programs
	# run as root (set-user-id) to check passwords, change accounts and
	# install Vexa for the administrators who start them.
	chmod 0600 $(BUILD)/rootfs/etc/shadow
	chmod 4755 $(addprefix $(BUILD)/rootfs/bin/,$(SETUID_PROGRAMS))
	tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime=@0 --sort=name \
		-cf $@ -C $(BUILD)/rootfs .

# ---- BusyBox ----

$(BUSYBOX_SRC)/Makefile:
	rm -rf $(BUSYBOX_SRC)
	git clone --depth 1 --branch $(BUSYBOX_VERSION) $(BUSYBOX_REPO) $(BUSYBOX_SRC)

# musl-gcc only searches musl's headers; BusyBox also needs the Linux kernel's
# (from linux-libc-dev), so make a directory with just those.
$(BUILD)/linux-headers:
	rm -rf $@ && mkdir -p $@
	for dir in linux asm-generic mtd drm; do ln -s /usr/include/$$dir $@/$$dir; done
	ln -s /usr/include/$$($(CC) -dumpmachine)/asm $@/asm

$(BUSYBOX): $(BUSYBOX_SRC)/Makefile third_party/busybox.config tools/configure-busybox.sh \
		| $(BUILD)/linux-headers
	mkdir -p $(BUSYBOX_BUILD)
	$(MAKE) -C $(BUSYBOX_SRC) O=$(abspath $(BUSYBOX_BUILD)) defconfig >/dev/null
	tools/configure-busybox.sh $(BUSYBOX_BUILD)/.config third_party/busybox.config
	yes '' | $(MAKE) -C $(BUSYBOX_BUILD) oldconfig >/dev/null
	$(MAKE) -C $(BUSYBOX_BUILD) CC=$(MUSL_CC) \
		EXTRA_CFLAGS="-isystem $(abspath $(BUILD)/linux-headers)" busybox busybox.links
	touch $@

busybox: $(BUSYBOX)

$(BUSYBOX_SOURCE_TARBALL): $(BUSYBOX)
	git -C $(BUSYBOX_SRC) archive --format=tar --prefix=busybox-$(BUSYBOX_VERSION)/ HEAD \
		> $(BUILD)/busybox-source.tar
	tar --append -f $(BUILD)/busybox-source.tar --transform 's,^,busybox-$(BUSYBOX_VERSION)/,' \
		-C $(BUSYBOX_BUILD) .config
	tar --append -f $(BUILD)/busybox-source.tar --transform 's,^third_party/,busybox-$(BUSYBOX_VERSION)/vexa-,' \
		third_party/busybox.config
	gzip -9n < $(BUILD)/busybox-source.tar > $@
	rm $(BUILD)/busybox-source.tar

busybox-source: $(BUSYBOX_SOURCE_TARBALL)

# ---- bash ----

$(BASH_TARBALL):
	mkdir -p $(dir $@)
	curl -fsSL -o $@.part $(BASH_URL)
	echo "$(BASH_SHA256)  $@.part" | sha256sum -c --quiet
	mv $@.part $@

# musl-gcc sees no ncurses, so bash uses its own bundled termcap and readline.
$(BASH): $(BASH_TARBALL)
	rm -rf $(BASH_BUILD) && mkdir -p $(BUILD)
	tar -xJf $(BASH_TARBALL) -C $(BUILD)
	cd $(BASH_BUILD) && ./configure CC=$(MUSL_CC) --prefix=/usr --without-bash-malloc \
		--disable-nls --enable-static-link=no > configure.log
	$(MAKE) -C $(BASH_BUILD) > $(BASH_BUILD)/build.log
	strip $@

bash: $(BASH)

# ---- coreutils ----

$(COREUTILS_TARBALL):
	mkdir -p $(dir $@)
	curl -fsSL -o $@.part $(COREUTILS_URL)
	echo "$(COREUTILS_SHA256)  $@.part" | sha256sum -c --quiet
	mv $@.part $@

$(COREUTILS): $(COREUTILS_TARBALL)
	rm -rf $(COREUTILS_BUILD) && mkdir -p $(BUILD)
	tar -xJf $(COREUTILS_TARBALL) -C $(BUILD)
	cd $(COREUTILS_BUILD) && FORCE_UNSAFE_CONFIGURE=1 ./configure CC=$(MUSL_CC) --prefix=/usr \
		--disable-nls --enable-single-binary=symlinks --without-selinux --disable-acl \
		--disable-xattr --without-libgmp --without-openssl \
		--enable-no-install-program=stdbuf > configure.log
	$(MAKE) -C $(COREUTILS_BUILD) > $(COREUTILS_BUILD)/build.log
	strip $@
	./$@ --help | sed -n '/Built-in programs/,/^$$/p' | tail -n +2 | tr ' ' '\n' | \
		grep -v '^$$' | sed 's/^ginstall$$/install/' > $(COREUTILS_BUILD)/programs.txt

coreutils: $(COREUTILS)

# ---- zlib and Python ----

# $(call fetch,url,sha256): downloads to $@, checked against the checksum.
define fetch
	mkdir -p $(dir $@)
	curl -fsSL -o $@.part $(1)
	echo "$(2)  $@.part" | sha256sum -c --quiet
	mv $@.part $@
endef

$(ZLIB_TARBALL):
	$(call fetch,$(ZLIB_URL),$(ZLIB_SHA256))

$(ZLIB): $(ZLIB_TARBALL)
	rm -rf $(BUILD)/zlib-1.3.dfsg $(ZLIB_PREFIX) && mkdir -p $(BUILD)
	tar -xJf $(ZLIB_TARBALL) -C $(BUILD)
	cd $(BUILD)/zlib-1.3.dfsg && CC=$(MUSL_CC) CFLAGS="-O2 -fPIC" ./configure --static \
		--prefix=$(abspath $(ZLIB_PREFIX)) > /dev/null
	$(MAKE) -C $(BUILD)/zlib-1.3.dfsg install > /dev/null

$(LIBFFI_TARBALL):
	$(call fetch,$(LIBFFI_URL),$(LIBFFI_SHA256))

$(LIBFFI): $(LIBFFI_TARBALL) | $(BUILD)/linux-headers
	rm -rf $(BUILD)/libffi-$(LIBFFI_VERSION) $(LIBFFI_PREFIX) && mkdir -p $(BUILD)
	tar -xzf $(LIBFFI_TARBALL) -C $(BUILD)
	cd $(BUILD)/libffi-$(LIBFFI_VERSION) && CC=$(MUSL_CC) \
		CFLAGS="-O2 -fPIC -isystem $(abspath $(BUILD)/linux-headers)" ./configure \
		--disable-shared --enable-static --disable-docs \
		--prefix=$(abspath $(LIBFFI_PREFIX)) > configure.log
	$(MAKE) -C $(BUILD)/libffi-$(LIBFFI_VERSION) install > /dev/null

$(OPENSSL_TARBALL):
	$(call fetch,$(OPENSSL_URL),$(OPENSSL_SHA256))

$(OPENSSL): $(OPENSSL_TARBALL) | $(BUILD)/linux-headers
	rm -rf $(OPENSSL_BUILD) $(OPENSSL_ROOT) && mkdir -p $(BUILD)
	tar -xzf $(OPENSSL_TARBALL) -C $(BUILD)
	cd $(OPENSSL_BUILD) && CC=$(MUSL_CC) ./Configure linux-x86_64 --prefix=/usr --libdir=lib \
		--openssldir=/etc/ssl shared no-tests no-async no-engine \
		-isystem $(abspath $(BUILD)/linux-headers) > configure.log
	$(MAKE) -C $(OPENSSL_BUILD) -j$$(nproc) > $(OPENSSL_BUILD)/build.log 2>&1
	$(MAKE) -C $(OPENSSL_BUILD) install_sw DESTDIR=$(abspath $(OPENSSL_ROOT)) \
		> $(OPENSSL_BUILD)/install.log 2>&1
	# (Its pkg-config files, for building curl and Python, point at the copy here.)
	sed -i 's|^prefix=.*|prefix=$(abspath $(OPENSSL_ROOT))/usr|' $(OPENSSL_ROOT)/usr/lib/pkgconfig/*.pc
	install -D -m 644 $(OPENSSL_BUILD)/apps/openssl.cnf $(OPENSSL_ROOT)/etc/ssl/openssl.cnf
	touch $@

openssl: $(OPENSSL)

$(CURL_TARBALL):
	$(call fetch,$(CURL_URL),$(CURL_SHA256))

$(CURL): $(CURL_TARBALL) $(OPENSSL) $(ZLIB) | $(BUILD)/linux-headers
	rm -rf $(CURL_BUILD) && mkdir -p $(BUILD)
	tar -xzf $(CURL_TARBALL) -C $(BUILD)
	cd $(CURL_BUILD) && PKG_CONFIG_LIBDIR= PKG_CONFIG_PATH= CC=$(MUSL_CC) \
		CPPFLAGS="-isystem $(abspath $(BUILD)/linux-headers)" ./configure \
		--host=x86_64-linux-musl --prefix=/usr --disable-shared --enable-static \
		--with-openssl=$(abspath $(OPENSSL_ROOT))/usr --with-zlib=$(abspath $(ZLIB_PREFIX)) \
		--with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --with-ca-path=/etc/ssl/certs \
		--without-libpsl --without-brotli --without-zstd --without-libidn2 \
		--without-nghttp2 --disable-ldap --disable-manual > configure.log
	$(MAKE) -C $(CURL_BUILD) -j$$(nproc) > $(CURL_BUILD)/build.log 2>&1
	install -D $(CURL_BUILD)/src/curl $@

curl: $(CURL)

$(ALSA_LIB_TARBALL):
	$(call fetch,$(ALSA_LIB_URL),$(ALSA_LIB_SHA256))

$(ALSA_UTILS_TARBALL):
	$(call fetch,$(ALSA_UTILS_URL),$(ALSA_UTILS_SHA256))

$(ALSA): $(ALSA_LIB_TARBALL) $(ALSA_UTILS_TARBALL) | $(BUILD)/linux-headers
	rm -rf $(BUILD)/alsa-lib-$(ALSA_LIB_VERSION) $(BUILD)/alsa-utils-$(ALSA_UTILS_VERSION) \
		$(ALSA_ROOT) && mkdir -p $(BUILD)
	tar -xjf $(ALSA_LIB_TARBALL) -C $(BUILD)
	cd $(BUILD)/alsa-lib-$(ALSA_LIB_VERSION) && CC=$(MUSL_CC) \
		CFLAGS="-O2 -isystem $(abspath $(BUILD)/linux-headers)" ./configure --prefix=/usr \
		--disable-static --enable-shared --disable-python --disable-ucm --disable-topology \
		--without-debug > configure.log
	$(MAKE) -C $(BUILD)/alsa-lib-$(ALSA_LIB_VERSION) -j$$(nproc) > /dev/null
	$(MAKE) -C $(BUILD)/alsa-lib-$(ALSA_LIB_VERSION) install DESTDIR=$(abspath $(ALSA_ROOT)) \
		> /dev/null
	tar -xjf $(ALSA_UTILS_TARBALL) -C $(BUILD)
	cd $(BUILD)/alsa-utils-$(ALSA_UTILS_VERSION) && CC=$(MUSL_CC) PKG_CONFIG_LIBDIR= \
		CFLAGS="-O2 -isystem $(abspath $(BUILD)/linux-headers) -I$(abspath $(ALSA_ROOT))/usr/include" \
		LDFLAGS="-L$(abspath $(ALSA_ROOT))/usr/lib" ./configure --prefix=/usr \
		--with-alsa-prefix=$(abspath $(ALSA_ROOT))/usr/lib \
		--with-alsa-inc-prefix=$(abspath $(ALSA_ROOT))/usr/include \
		--disable-alsamixer --disable-xmlto --disable-rst2man --disable-nls --disable-alsaconf \
		--disable-alsatest --disable-bat --with-systemdsystemunitdir=no \
		--with-udev-rules-dir=/tmp > configure.log
	$(MAKE) -C $(BUILD)/alsa-utils-$(ALSA_UTILS_VERSION)/aplay > /dev/null
	$(MAKE) -C $(BUILD)/alsa-utils-$(ALSA_UTILS_VERSION)/speaker-test > /dev/null
	install -D $(BUILD)/alsa-utils-$(ALSA_UTILS_VERSION)/aplay/aplay \
		$(BUILD)/alsa-utils-$(ALSA_UTILS_VERSION)/speaker-test/speaker-test -t $(ALSA_ROOT)/usr/bin
	touch $@

alsa: $(ALSA)

$(PYTHON_TARBALL):
	$(call fetch,$(PYTHON_URL),$(PYTHON_SHA256))

# Built with musl like the rest; host pkg-config is kept out so only libraries
# built here are used. The build runs its own python on the host (which has
# musl's loader), and optional modules without their libraries are skipped.
$(PYTHON): $(PYTHON_TARBALL) $(ZLIB) $(LIBFFI) $(OPENSSL) tools/prune-python.sh \
		| $(BUILD)/linux-headers
	rm -rf $(PYTHON_BUILD) $(PYTHON_ROOT) && mkdir -p $(BUILD)
	tar -xJf $(PYTHON_TARBALL) -C $(BUILD)
	cd $(PYTHON_BUILD) && \
		PKG_CONFIG_LIBDIR=$(abspath $(ZLIB_PREFIX))/lib/pkgconfig:$(abspath $(LIBFFI_PREFIX))/lib/pkgconfig \
		PKG_CONFIG_PATH= MUSL_CC=$(MUSL_CC) ./configure \
		CC=$(abspath tools/musl-cc-wrapper.sh) --prefix=/usr --without-ensurepip \
		--disable-test-modules --with-computed-gotos \
		--with-openssl=$(abspath $(OPENSSL_ROOT))/usr --with-openssl-rpath=no \
		CPPFLAGS="-I$(abspath $(ZLIB_PREFIX))/include -I$(abspath $(LIBFFI_PREFIX))/include \
		-isystem $(abspath $(BUILD)/linux-headers)" \
		LDFLAGS="-L$(abspath $(ZLIB_PREFIX))/lib -L$(abspath $(LIBFFI_PREFIX))/lib" > configure.log
	# (The build tries its modules, so they need to find OpenSSL here.)
	MUSL_CC=$(MUSL_CC) LD_LIBRARY_PATH=$(abspath $(OPENSSL_ROOT))/usr/lib \
		$(MAKE) -C $(PYTHON_BUILD) -j$$(nproc) > $(PYTHON_BUILD)/build.log 2>&1
	MUSL_CC=$(MUSL_CC) LD_LIBRARY_PATH=$(abspath $(OPENSSL_ROOT))/usr/lib \
		$(MAKE) -C $(PYTHON_BUILD) install DESTDIR=$(abspath $(PYTHON_ROOT)) \
		> $(PYTHON_BUILD)/install.log 2>&1
	tools/prune-python.sh $(PYTHON_ROOT) $(basename $(PYTHON_VERSION))
	touch $@

python: $(PYTHON)

# The sources of the Linux programs built above (for CI, which keeps their builds).
linux-tarballs: $(ZLIB_TARBALL) $(LIBFFI_TARBALL) $(OPENSSL_TARBALL) $(CURL_TARBALL) \
	$(ALSA_LIB_TARBALL) $(ALSA_UTILS_TARBALL) $(PYTHON_TARBALL)

# ---- /linux ----

# Linux test programs (tests/linux/), built with musl like the rest of /linux.
LINUX_TESTS := $(patsubst tests/linux/%.c,$(BUILD)/linux-tests/%,$(wildcard tests/linux/*.c)) \
	$(wildcard tests/linux/*.py)

$(BUILD)/linux-tests/%: tests/linux/%.c | $(BUILD)/linux-headers
	@mkdir -p $(dir $@)
	$(MUSL_CC) -O2 -Wall -Wextra -Werror -pthread -isystem $(BUILD)/linux-headers $< -o $@

$(X11): $(X11_SOURCES) tools/build-x11.sh $(wildcard third_party/xvexa/*) | $(BUILD)/linux-headers
	tools/build-x11.sh $(X11_SOURCES) third_party/x11 $(BUILD)/x11-work $(X11_SYSROOT) \
		$(BUILD)/linux-headers
	touch $@

x11: $(X11)

$(LLVM_TARBALL):
	$(call fetch,$(LLVM_URL),$(LLVM_SHA256))

$(MESA_TARBALL):
	$(call fetch,$(MESA_URL),$(MESA_SHA256))

$(MESA): $(LLVM_TARBALL) $(MESA_TARBALL) tools/build-mesa.sh tools/musl-libcxx-wrapper.sh \
		$(X11) | $(BUILD)/linux-headers
	tools/build-mesa.sh $(LLVM_TARBALL) $(MESA_TARBALL) $(BUILD)/mesa-work $(X11_SYSROOT) \
		$(MESA_ROOT) $(BUILD)/linux-headers
	touch $@

mesa: $(MESA)

# gl-test: draws with OpenGL (through OSMesa, or GLX in an X window).
$(GL_TEST): tests/gl/gl-test.c $(MESA)
	@mkdir -p $(dir $@)
	$(MUSL_CC) -O2 -Wall -Wextra -Werror -I$(MESA_ROOT)/usr/include -I$(X11_SYSROOT)/usr/include \
		-L$(MESA_ROOT)/usr/lib -L$(X11_SYSROOT)/usr/lib -Wl,-rpath-link,$(MESA_ROOT)/usr/lib \
		-Wl,-rpath-link,$(X11_SYSROOT)/usr/lib $< -o $@ -lOSMesa -lGL -lX11 -lunwind

$(LINUX_ROOT)/.done: $(BUSYBOX) $(BASH) $(COREUTILS) $(PYTHON) $(X11) $(MUSL_LIBC) $(LINUX_TESTS) \
		$(XCLIPBOARD) $(OPENSSL) $(CURL) $(MESA) $(GL_TEST) $(ALSA) tools/make-linux-root.sh \
		$(wildcard tools/linux-files/* tools/linux-files/applications/* tools/linux-files/gtk-theme/*)
	tools/make-linux-root.sh $(LINUX_ROOT) $(MUSL_LIBC) $(BUSYBOX) \
		$(BUSYBOX_BUILD)/busybox.links $(BASH) $(COREUTILS) $(COREUTILS_BUILD)/programs.txt \
		$(PYTHON_ROOT) $(X11_SYSROOT) $(LINUX_TESTS)
	cp $(XCLIPBOARD) $(LINUX_ROOT)/usr/bin/xclipboard
	# TLS: OpenSSL's libraries and openssl, curl, and the root certificates
	# (tools/make-ca-bundle.py) where OpenSSL and curl look for them.
	cp -a $(OPENSSL_ROOT)/usr/lib/libssl.so* $(OPENSSL_ROOT)/usr/lib/libcrypto.so* \
		$(LINUX_ROOT)/usr/lib/
	strip --strip-unneeded $(LINUX_ROOT)/usr/lib/libssl.so.3 $(LINUX_ROOT)/usr/lib/libcrypto.so.3
	install -s $(OPENSSL_ROOT)/usr/bin/openssl $(CURL) $(LINUX_ROOT)/usr/bin/
	mkdir -p $(LINUX_ROOT)/etc/ssl/certs
	cp tools/linux-files/ca-certificates.crt $(LINUX_ROOT)/etc/ssl/certs/
	ln -sf certs/ca-certificates.crt $(LINUX_ROOT)/etc/ssl/cert.pem
	cp $(OPENSSL_ROOT)/etc/ssl/openssl.cnf $(LINUX_ROOT)/etc/ssl/
	# OpenGL (tools/build-mesa.sh): Mesa's libraries, LLVM, LLVM's C++ runtime.
	cp -a $(MESA_ROOT)/usr/lib/libc++.so.1* $(MESA_ROOT)/usr/lib/libc++abi.so.1* \
		$(MESA_ROOT)/usr/lib/libunwind.so.1* $(MESA_ROOT)/usr/lib/libLLVM*.so* \
		$(MESA_ROOT)/usr/lib/libGL.so* $(MESA_ROOT)/usr/lib/libOSMesa.so* \
		$(MESA_ROOT)/usr/lib/libglapi.so* $(MESA_ROOT)/usr/lib/libGLESv2.so* $(LINUX_ROOT)/usr/lib/
	find $(LINUX_ROOT)/usr/lib -maxdepth 1 \( -name 'libLLVM*' -o -name 'libGL*' -o \
		-name 'libOSMesa*' -o -name 'libglapi*' -o -name 'libc++*' -o -name 'libunwind*' \) \
		-type f -exec strip --strip-unneeded {} +
	install -s $(GL_TEST) $(LINUX_ROOT)/usr/bin/
	# Sound: ALSA's library and its settings, aplay (and arecord), speaker-test.
	cp -a $(ALSA_ROOT)/usr/lib/libasound.so.2* $(LINUX_ROOT)/usr/lib/
	cp -a $(ALSA_ROOT)/usr/share/alsa $(LINUX_ROOT)/usr/share/
	cp tools/linux-files/asound.conf $(LINUX_ROOT)/etc/asound.conf
	install -s $(ALSA_ROOT)/usr/bin/aplay $(ALSA_ROOT)/usr/bin/speaker-test $(LINUX_ROOT)/usr/bin/
	ln -sf aplay $(LINUX_ROOT)/usr/bin/arecord
	touch $@

# xclipboard: the clipboard between X programs and Vexa's (an X program,
# started by xrun with the X server).
$(XCLIPBOARD): tools/xclipboard.c $(X11)
	@mkdir -p $(dir $@)
	$(MUSL_CC) -O2 -Wall -I$(X11_SYSROOT)/usr/include -L$(X11_SYSROOT)/usr/lib \
		-Wl,-rpath-link,$(X11_SYSROOT)/usr/lib $< -o $@ -lXfixes -lX11

$(BUILD)/disk-content: $(DISK_CONTENT_FILES) $(BUILD)/programs/hello-world $(SDK_HELLO)
	rm -rf $@
	cp -R tests/disk-content $@
	cp $(BUILD)/programs/hello-world $@/
	cp -R $(SDK_TEST)/hello-app/build/HelloSDK.vxapp $(SDK_SDL_DEMO) $@/
	rm -f $@/HelloSDK.vxapp/.done

$(BUILD)/disks/virtio-gpt.img: $(BUILD)/disk-content tools/make-disk.py
	@mkdir -p $(dir $@)
	tools/make-disk.py $@ 32 gpt $<

$(BUILD)/disks/sata-mbr.img: $(BUILD)/disk-content tools/make-disk.py
	@mkdir -p $(dir $@)
	tools/make-disk.py $@ 24 mbr $<

$(BUILD)/disks/nvme-whole.img: $(BUILD)/disk-content tools/make-disk.py
	@mkdir -p $(dir $@)
	tools/make-disk.py $@ 16 none $< ext3

$(BUILD)/disks/ext4.img: tools/make-ext4-disk.sh
	@mkdir -p $(dir $@)
	tools/make-ext4-disk.sh $@

$(BUILD)/disks/fat32.img: tools/make-fat-disks.sh
	@mkdir -p $(dir $@)
	tools/make-fat-disks.sh $@ $(BUILD)/disks/exfat.img

$(BUILD)/disks/exfat.img: $(BUILD)/disks/fat32.img

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
	rm -rf $(2).root
	mkdir -p $(2).root/boot/limine $(2).root/EFI/BOOT
	cp $(KERNEL) $(2).root/boot/
	cp $(INITRAMFS) $(2).root/boot/
	cp $(1) $(2).root/boot/limine/limine.conf
	cp limine/limine-bios.sys limine/limine-bios-cd.bin \
		limine/limine-uefi-cd.bin $(2).root/boot/limine/
	cp limine/BOOTX64.EFI limine/BOOTIA32.EFI $(2).root/EFI/BOOT/
	if [ -n "$(LINUX_TREE)" ]; then mkdir -p $(2).root/linux && \
		for dir in $(LINUX_ON_CD); do cp -a $(LINUX_ROOT)/$$dir $(2).root/linux/; done; fi
	mkdir -p $(2).root/lib
	cp $(MESA_VEXA_LIB) $(2).root/lib/
	mkdir -p $(2).root/doom
	cp $(FREEDOOM_DIR)/freedoom1.wad $(FREEDOOM_DIR)/COPYING.txt $(FREEDOOM_DIR)/CREDITS.txt \
		$(2).root/doom/
	xorriso -as mkisofs -R -r -J -D -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(2).root -o $(2) 2>/dev/null
	./limine/limine bios-install $(2)
	rm -rf $(2).root
endef

$(ISO): $(KERNEL) $(INITRAMFS) $(BUILD)/limine.conf limine/limine $(LINUX_TREE) \
		$(FREEDOOM_DIR)/freedoom1.wad $(MESA_VEXA_LIB)
	$(call make_iso,$(BUILD)/limine.conf,$@)

$(SAFE_ISO): $(KERNEL) $(INITRAMFS) $(BUILD)/limine.conf limine/limine $(FREEDOOM_DIR)/freedoom1.wad \
		$(MESA_VEXA_LIB)
	{ echo 'default_entry: 2'; cat $(BUILD)/limine.conf; } > $(BUILD)/limine-safe-mode.conf
	$(call make_iso,$(BUILD)/limine-safe-mode.conf,$@)

run: $(ISO)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -serial stdio -no-reboot $(QEMU_NET) $(QEMU_AUDIO) \
		$(QEMU_USB)

# Like run, with a virtio disk mounted at /mnt/vda1. It keeps what you write;
# delete build/my-disk.img to start over.
run-disk: $(ISO) $(MY_DISK)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -boot d -serial stdio -no-reboot $(QEMU_NET) $(QEMU_AUDIO) \
		-drive file=$(MY_DISK),if=virtio,format=raw $(QEMU_USB)

# Like run, with QEMU's virtio GPU with 3D (virgl): native OpenGL programs
# draw on this machine's GPU. (QEMU needs its GTK display with OpenGL.)
run-virgl: $(ISO)
	$(QEMU) -M q35 -m 1G -cdrom $(ISO) -serial stdio -no-reboot $(QEMU_NET) $(QEMU_AUDIO) \
		$(QEMU_USB) -device virtio-gpu-gl-pci -display gtk,gl=on

run-nographic: $(ISO)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -nographic -no-reboot $(QEMU_NET) $(QEMU_AUDIO)

# `make test` boots Vexa four ways at once (each in its own QEMU, with KVM
# when this machine has it); each one's output is shown when it's done.
TEST_JOBS ?= 4
test: $(ISO) $(SAFE_ISO) $(TEST_DISKS) native-iso
	$(MAKE) --no-print-directory --output-sync=target -j$(TEST_JOBS) \
		test-bios test-uefi test-safe test-native-boot test-install

# OpenGL on the virtio GPU (under Xvfb when there's no display). Not part of
# `make test`: it needs QEMU with virgl and OpenGL on this machine.
test-virgl: $(ISO)
	tools/qemu-smoke-test.py --only virgl --virgl

test-bios:
	tools/qemu-smoke-test.py --disks $(BUILD)/disks
test-uefi:
	tools/qemu-smoke-test.py --disks $(BUILD)/disks --uefi --smp 4 --memory 6G --cpu max --usb \
		--nic e1000e
test-safe:
	tools/qemu-smoke-test.py --disks $(BUILD)/disks --safe-mode --iso $(SAFE_ISO) --nic e1000 \
		--usb ehci

# Vexa must work without the Linux subsystem: build and boot a kernel without it.
native-iso: $(DOOM) $(CXX_TEST) $(CXX_TEST_STATIC) $(MESA_VEXA) $(SDL_GL_TEST) $(NETSURF) \
		$(BUILD)/freedoom/freedoom1.wad
	$(MAKE) BUILD=$(BUILD)/native LINUX_COMPAT=0 DOOM_PROGRAM=$(abspath $(DOOM)) \
		CXX_TEST_PROGRAM="$(abspath $(CXX_TEST) $(CXX_TEST_STATIC))" \
		MESA_VEXA_LIB=$(abspath $(MESA_VEXA)) SDL_GL_TEST_PROGRAM=$(abspath $(SDL_GL_TEST)) \
		NETSURF_PROGRAM=$(abspath $(NETSURF)) NETSURF_RES=$(abspath $(NETSURF_DIR)/res) \
		FREEDOOM_DIR=$(abspath $(BUILD)/freedoom) iso
# Installing on a disk (the Installer app), then starting from it.
test-install: $(ISO)
	tools/install-test.sh

test-native-boot:
	tools/qemu-smoke-test.py --no-linux --iso $(BUILD)/native/vexa.iso --usb ohci --machine pc
test-native: native-iso
	$(MAKE) --no-print-directory test-native-boot

# One boot, some of the checks: `make test-quick ONLY=desktop` (sections:
# shell, network, desktop, linux, x, linux-net, disks; several with commas).
ONLY ?= desktop
test-quick: $(ISO) $(TEST_DISKS)
	tools/qemu-smoke-test.py --disks $(BUILD)/disks --only $(ONLY)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf limine $(BUSYBOX_SRC) $(BASH_TARBALL) $(COREUTILS_TARBALL) $(ZLIB_TARBALL) \
		$(LIBFFI_TARBALL) $(PYTHON_TARBALL)

-include $(OBJS:.o=.d) $(USER_OBJS:.o=.d) $(VEXA_LD_OBJS:.o=.d)
