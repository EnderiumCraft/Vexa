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

LIBVEXA_SRCS := $(wildcard libvexa/src/*.c libvexa/src/*.S)
LIBVEXA_OBJS := $(patsubst libvexa/src/%,$(BUILD)/libvexa/%.o,$(LIBVEXA_SRCS))
CRT0 := $(BUILD)/libvexa/crt0.S.o
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
CURL := $(CURL_BUILD)/src/curl

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

# Test disks for `make test`: the same ext2 file system (tests/disk-content
# plus hello-world) on three kinds of disk, each with a different layout.
DISK_CONTENT_FILES := $(shell find tests/disk-content -type f)
TEST_DISKS := $(BUILD)/disks/virtio-gpt.img $(BUILD)/disks/sata-mbr.img \
	$(BUILD)/disks/nvme-whole.img
# A disk for `make run-disk`, created once and kept, so changes survive reboots.
MY_DISK := $(BUILD)/my-disk.img
USER_OBJS := $(LIBVEXA_OBJS) \
	$(patsubst %,$(BUILD)/%.o,$(wildcard $(addsuffix /*.c,$(addprefix userland/,$(PROGRAMS)))))

.PHONY: all openssl curl mesa alsa kernel programs iso run run-disk run-nographic test test-disks clean distclean \
	busybox busybox-source bash coreutils python x11 test-native test-quick native-iso \
	test-bios test-uefi test-safe test-native-boot

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

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/libvexa/%.o: libvexa/src/%
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/userland/%.c.o: userland/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# libvexa.so: libvexa without crt0 (which each program carries), binding its
# own references to itself.
$(LIBVEXA_SO): $(filter-out $(CRT0),$(LIBVEXA_OBJS))
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -shared -Bsymbolic -soname libvexa.so $^ -o $@

# The dynamic loader relocates itself before it uses any pointer, so all its
# symbols are hidden: it needs only relative relocations.
$(BUILD)/vexa-ld/%.o: libvexa/ld/%
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -fvisibility=hidden -c $< -o $@

$(VEXA_LD): $(VEXA_LD_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -shared -Bsymbolic -e _start $^ -o $@

.SECONDEXPANSION:
$(BUILD)/programs/%: $(CRT0) $(LIBVEXA_SO) $(LIBVEXA_OBJS) libvexa/program.ld \
		$$(addprefix $(BUILD)/,$$(addsuffix .o,$$(wildcard userland/$$*/*.c)))
	@mkdir -p $(dir $@)
	$(if $(filter $*,$(STATIC_PROGRAMS)), \
		$(LD) $(STATIC_LDFLAGS) $(sort $(filter %.o,$^)) -o $@, \
		$(LD) $(DYNAMIC_LDFLAGS) $(CRT0) $(filter-out $(LIBVEXA_OBJS),$(filter %.o,$^)) \
			$(LIBVEXA_SO) -o $@)

# /linux is split: what's written to stays in memory, the rest is read from
# the boot CD when it's used (the ISO's /linux; /cdrom is the boot CD).
LINUX_IN_MEMORY := etc var root
LINUX_ON_CD := bin lib sbin usr

# The starting root file system: rootfs/ plus the programs in /bin and the
# apps in /apps, as a tar archive (ustar, with fixed owners and times so
# builds are reproducible).
$(INITRAMFS): $(PROGRAM_BINS) $(LIBVEXA_SO) $(VEXA_LD) $(ROOTFS_FILES) $(APP_FILES) $(LINUX_TREE) \
		docs/USER-GUIDE.md
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin $(BUILD)/rootfs/lib
	cp -R rootfs/. $(BUILD)/rootfs/
	# The Help app's book: the user guide.
	mkdir -p $(BUILD)/rootfs/share/help
	cp docs/USER-GUIDE.md $(BUILD)/rootfs/share/help/
	cp $(PROGRAM_BINS) $(BUILD)/rootfs/bin/
	cp $(LIBVEXA_SO) $(VEXA_LD) $(BUILD)/rootfs/lib/
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
	# /linux: the files that change (etc, var, root) are here, in memory;
	# the programs and libraries stay on the boot CD (see make_iso).
	if [ -n "$(LINUX_TREE)" ]; then \
		mkdir -p $(BUILD)/rootfs/linux && \
		for dir in $(LINUX_IN_MEMORY); do cp -a $(LINUX_ROOT)/$$dir $(BUILD)/rootfs/linux/; done && \
		for dir in $(LINUX_ON_CD); do ln -s /cdrom/linux/$$dir $(BUILD)/rootfs/linux/$$dir; done; fi
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
		$(wildcard tools/linux-files/* tools/linux-files/applications/*)
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
	cp $(OPENSSL_BUILD)/apps/openssl.cnf $(LINUX_ROOT)/etc/ssl/
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
	install -s $(BUILD)/alsa-utils-$(ALSA_UTILS_VERSION)/aplay/aplay \
		$(BUILD)/alsa-utils-$(ALSA_UTILS_VERSION)/speaker-test/speaker-test $(LINUX_ROOT)/usr/bin/
	ln -sf aplay $(LINUX_ROOT)/usr/bin/arecord
	touch $@

# xclipboard: the clipboard between X programs and Vexa's (an X program,
# started by xrun with the X server).
$(XCLIPBOARD): tools/xclipboard.c $(X11)
	@mkdir -p $(dir $@)
	$(MUSL_CC) -O2 -Wall -I$(X11_SYSROOT)/usr/include -L$(X11_SYSROOT)/usr/lib \
		-Wl,-rpath-link,$(X11_SYSROOT)/usr/lib $< -o $@ -lXfixes -lX11

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
	if [ -n "$(LINUX_TREE)" ]; then mkdir -p $(BUILD)/iso_root/linux && \
		for dir in $(LINUX_ON_CD); do cp -a $(LINUX_ROOT)/$$dir $(BUILD)/iso_root/linux/; done; fi
	xorriso -as mkisofs -R -r -J -D -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BUILD)/iso_root -o $(2) 2>/dev/null
	./limine/limine bios-install $(2)
endef

$(ISO): $(KERNEL) $(INITRAMFS) $(BUILD)/limine.conf limine/limine $(LINUX_TREE)
	$(call make_iso,$(BUILD)/limine.conf,$@)

$(SAFE_ISO): $(KERNEL) $(INITRAMFS) $(BUILD)/limine.conf limine/limine
	{ echo 'default_entry: 2'; cat $(BUILD)/limine.conf; } > $(BUILD)/limine-safe-mode.conf
	$(call make_iso,$(BUILD)/limine-safe-mode.conf,$@)

run: $(ISO)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -serial stdio -no-reboot $(QEMU_NET) $(QEMU_AUDIO)

# Like run, with a virtio disk mounted at /mnt/vda1. It keeps what you write;
# delete build/my-disk.img to start over.
run-disk: $(ISO) $(MY_DISK)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -boot d -serial stdio -no-reboot $(QEMU_NET) $(QEMU_AUDIO) \
		-drive file=$(MY_DISK),if=virtio,format=raw

run-nographic: $(ISO)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -nographic -no-reboot $(QEMU_NET) $(QEMU_AUDIO)

# `make test` boots Vexa four ways at once (each in its own QEMU, with KVM
# when this machine has it); each one's output is shown when it's done.
TEST_JOBS ?= 4
test: $(ISO) $(SAFE_ISO) $(TEST_DISKS) native-iso
	$(MAKE) --no-print-directory --output-sync=target -j$(TEST_JOBS) \
		test-bios test-uefi test-safe test-native-boot

test-bios:
	tools/qemu-smoke-test.py --disks $(BUILD)/disks
test-uefi:
	tools/qemu-smoke-test.py --disks $(BUILD)/disks --uefi --smp 4 --memory 6G --cpu max
test-safe:
	tools/qemu-smoke-test.py --disks $(BUILD)/disks --safe-mode --iso $(SAFE_ISO)

# Vexa must work without the Linux subsystem: build and boot a kernel without it.
native-iso:
	$(MAKE) BUILD=$(BUILD)/native LINUX_COMPAT=0 iso
test-native-boot:
	tools/qemu-smoke-test.py --no-linux --iso $(BUILD)/native/vexa.iso
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
