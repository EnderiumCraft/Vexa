# Vexa roadmap: from boot to Firefox

Firefox is one of the most demanding programs you can run. It needs, all at once: a full
POSIX environment, dynamic linking, hundreds of threads, several cooperating processes that
share memory and pass file descriptors to each other, a TCP/IP stack, a windowing system,
GTK 3, and a few gigabytes of RAM. Each phase below builds one layer of that, and each
ends with something you can see working.

## The key strategic decision: speak Linux's language

There are two ways to get Firefox onto a new kernel:

1. **Native port.** Write your own libc (or port [mlibc](https://github.com/managarm/mlibc)),
   then cross-compile everything for Vexa: GTK, glib, cairo, Mesa, X11 or Wayland, and
   Firefox itself, which also means teaching Rust and LLVM about a new target. That's
   dozens of ports, each with its own build system.
2. **Linux-compatible system call ABI (recommended).** Vexa keeps its own kernel design,
   but user programs call it with Linux's x86_64 system call numbers and semantics. Then
   ordinary Linux binaries run unmodified: first a static BusyBox, then a musl-based
   userland (for example Alpine Linux's packages), and at the end the Firefox build that
   your distribution already ships.

Vexa takes approach 2. It is still your own operating system: the kernel, memory manager,
scheduler, file systems, drivers and network stack are all written for Vexa. Compatibility
only sets the contract at the user/kernel boundary, so the years go into the kernel and
not into porting GTK. (You can still add native Vexa programs and APIs later.)

---

## Phase 1: Boot and CPU basics *(in progress)*

- [x] Boot with Limine on BIOS and UEFI, higher-half 64-bit kernel
- [x] Serial logging, `kprintf`, `panic`
- [x] GDT and IDT with CPU exception reporting
- [x] Read the memory map and framebuffer from the bootloader
- [ ] Text console on the framebuffer (bitmap font, scrolling)
- [ ] Parse ACPI tables (MADT) and set up the Local APIC and I/O APIC
- [ ] Timer (APIC timer calibrated against HPET or PIT)
- [ ] PS/2 keyboard driver

**Milestone:** type on the keyboard and see the characters on screen.

## Phase 2: Memory management

- [ ] Physical page allocator (free list or buddy allocator)
- [ ] Virtual memory manager: build Vexa's own page tables, map/unmap, drop Limine's tables
- [ ] Kernel heap (`kmalloc`/`kfree`, then a slab allocator)
- [ ] Page fault handler that can grow the kernel stack and later do demand paging

**Milestone:** allocate and free millions of objects without leaking or crashing.

## Phase 3: Processes, threads and user mode

- [ ] Kernel threads and a preemptive scheduler driven by the timer
- [ ] TSS, ring 3 segments, jumping to user mode
- [ ] `syscall`/`sysret` entry with Linux x86_64 syscall numbering
- [ ] ELF64 loader for static executables
- [ ] Save/restore FPU, SSE and AVX state (`XSAVE`). Firefox will crash in odd ways
      without this
- [ ] SMP: start the other CPU cores and make the scheduler multi-core safe

**Milestone:** a statically linked "hello world" built on Linux runs on Vexa.

## Phase 4: Files and storage

- [ ] VFS layer (inodes, dentries, mount points, file descriptors)
- [ ] initramfs loaded as a Limine module (cpio or tar)
- [ ] tmpfs, devfs (`/dev/null`, `/dev/tty`, `/dev/fb0`...), procfs
- [ ] PCI enumeration; virtio-blk driver (QEMU), then AHCI and NVMe for real hardware
- [ ] ext2 read/write (ext4 later)

**Milestone:** boot from a disk image and list files with a static `ls`.

## Phase 5: A real Unix userland

- [ ] `fork`, `execve`, `wait4`, `exit`, process groups and sessions
- [ ] `mmap`/`munmap`/`mprotect` with copy-on-write and file-backed mappings
- [ ] Signals, pipes, `dup2`, `fcntl`, `ioctl`, terminals and ptys
- [ ] `poll`/`select`, then `epoll` and `eventfd`
- [ ] Clocks: `clock_gettime`, `nanosleep`, timers
- [ ] Run static BusyBox as `/sbin/init` and get a shell

**Milestone:** a working shell on Vexa where you can run `ls`, `cat`, `vi`.

## Phase 6: Dynamic linking and threads

- [ ] Run the musl dynamic loader (`ld-musl-x86_64.so.1`) and shared libraries
- [ ] `clone` with thread flags, TLS via `arch_prctl(ARCH_SET_FS)`, `futex`, `set_tid_address`
- [ ] `getrandom`, `/proc/self/maps`, `sched_getaffinity`, `prctl`, rlimits
- [ ] Run Alpine Linux's `apk`-installed packages: bash, coreutils, python3

**Milestone:** Python runs a multithreaded script on Vexa.

## Phase 7: Networking

- [ ] virtio-net (QEMU) and Intel e1000 drivers
- [ ] TCP/IP stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP; DHCP client (IPv6 later)
- [ ] BSD sockets API, including **Unix domain sockets with `SCM_RIGHTS`
      file descriptor passing**. X11 and Firefox's multi-process IPC both depend on it
- [ ] DNS works via `/etc/resolv.conf`

**Milestone:** `wget https://example.com` succeeds from the Vexa shell.

## Phase 8: Graphics and input

- [ ] Mouse (PS/2), then USB: xHCI controller and HID keyboard/mouse
- [ ] Minimal DRM/KMS "dumb buffer" interface over the boot framebuffer
      (or a virtio-gpu driver in QEMU)
- [ ] evdev-style input devices under `/dev/input`
- [ ] Shared memory: `memfd_create`, `/dev/shm`, `MAP_SHARED`
- [ ] Run an X server (Xorg with the modesetting or fbdev driver), then a
      simple window manager and `xterm`

**Milestone:** a graphical desktop with windows you can drag around.

## Phase 9: The desktop stack

- [ ] fontconfig + FreeType render text in X clients
- [ ] GTK 3 demo (`gtk3-demo`) runs
- [ ] Mesa's software renderer (llvmpipe) for OpenGL, since Firefox's WebRender
      can fall back to software rendering anyway
- [ ] Audio (optional for first light): Intel HDA driver + ALSA-compatible interface

**Milestone:** GTK applications run on the Vexa desktop.

## Phase 10: Firefox

- [ ] Launch with the content sandbox disabled (`MOZ_DISABLE_CONTENT_SANDBOX=1`),
      because seccomp-bpf and user namespaces can come much later
- [ ] Fix syscalls it hits that Vexa does not implement yet (log unknown syscalls
      loudly from Phase 3 onwards; this becomes the to-do list)
- [ ] Multi-process: parent, content, GPU/RDD and socket processes all talking over
      Unix sockets and shared memory
- [ ] Performance: enough RAM (give QEMU 4 GiB or more), SMP, a decent page cache
- [ ] Load `https://www.mozilla.org` with working TLS

**Milestone: Firefox runs on Vexa.**

---

## Working habits that make this feasible

- **Test in QEMU on every change.** Add a CI job that boots the ISO headless and
  checks the serial log. `make run-nographic` is the starting point.
- **Log unknown syscalls** with their number and arguments instead of silently failing.
- **Use GDB with QEMU** (`-s -S`, then `target remote :1234`), since the kernel is built
  with `-g`.
- **Keep each phase shippable.** Finish a milestone before starting the next.
- **Run on real hardware** occasionally, starting after Phase 1. Emulators forgive a lot.

## References

- [OSDev Wiki](https://wiki.osdev.org/): the starting point for almost every topic here
- [Intel 64 and IA-32 Software Developer's Manuals](https://www.intel.com/sdm): CPU reference
- [Limine boot protocol](https://github.com/limine-bootloader/limine-protocol)
- [Linux syscall table for x86_64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)
  and the `man 2` pages for exact semantics
- *Operating Systems: Three Easy Pieces* (free online): concepts behind Phases 2 to 5
