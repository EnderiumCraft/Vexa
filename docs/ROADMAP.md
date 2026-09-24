# Vexa roadmap: from boot to Firefox

Firefox is one of the most demanding programs you can run. It needs, all at once: a full
POSIX environment, dynamic linking, hundreds of threads, several cooperating processes that
share memory and pass file descriptors to each other, a TCP/IP stack, a windowing system,
GTK 3, and a few gigabytes of RAM. Each phase below builds one layer of that, and each
ends with something you can see working.

## The key strategic decision: Vexa first, Linux compatibility on the side

Everything in Vexa is Vexa's own: the kernel, its system call interface, its C library
and its core programs. Linux compatibility is a separate, optional subsystem that translates
Linux system calls into Vexa's own kernel operations. The Firefox you download for
Linux runs through that subsystem, so GTK, Mesa and X11 never need porting.

The kernel can be built without the Linux subsystem and still be a complete operating
system. See [ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit together and the
rules that keep them apart.

This gives the roadmap two tracks that share one kernel:

- **Native track:** the Vexa system call interface, `libvexa` (Vexa's own C library),
  and programs written for Vexa: init, shell, core utilities, and later a desktop.
- **Linux track:** the Linux subsystem, which grows until Firefox runs on it.

Both tracks need the same kernel features (processes, memory, files, networking,
graphics), so work on one moves the other forward.

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
- [ ] One `syscall`/`sysret` entry point that hands each call to the calling process's
      personality (see [ARCHITECTURE.md](ARCHITECTURE.md))
- [ ] The first native Vexa system calls: `vx_log`, `vx_exit`
- [ ] ELF64 loader that picks the personality from the executable's `.note.vexa` note
- [ ] Save/restore FPU, SSE and AVX state (`XSAVE`). Firefox will crash in odd ways
      without this
- [ ] SMP: start the other CPU cores and make the scheduler multi-core safe

**Milestone:** a native Vexa "hello world" runs in user mode.

## Phase 4: Files and storage

- [ ] Handle table: one kernel object model for files, pipes, processes and more
- [ ] VFS layer (inodes, directory entries, mount points)
- [ ] initramfs loaded as a Limine module (tar)
- [ ] tmpfs and a device file system (`/dev/null`, `/dev/console`, `/dev/fb0`...)
- [ ] PCI enumeration; virtio-blk driver (QEMU), then AHCI and NVMe for real hardware
- [ ] ext2 read/write (ext4 later), so disks can be shared with other systems

**Milestone:** boot from a disk image and read files from it.

## Phase 5: Vexa userland

Native track:
- [ ] `libvexa`: C startup code, strings, memory allocator, `printf`, file I/O
      over native system calls
- [ ] Native process calls: spawn, wait, exit; memory mapping; pipes
- [ ] `vinit` (first process) and `vsh` (the Vexa shell)
- [ ] Core utilities written for Vexa: `ls`, `cat`, `echo`, `mkdir`, `rm`

Linux track:
- [ ] Create `kernel/src/personality/linux/` and the `LINUX_COMPAT` build option
- [ ] Linux calls mapped onto the core: `read`, `write`, `open`, `mmap`, `exit_group`,
      then `fork`, `execve`, `wait4`, signals, `ioctl` for terminals
- [ ] Log every unimplemented Linux call with its number and arguments
- [ ] Run static BusyBox from the Vexa shell

**Milestone:** a working Vexa shell, and Linux BusyBox running from it.

## Phase 6: Dynamic linking and threads

- [ ] Threads, thread-local storage, and a core "wait on address" primitive
      (native API first; the Linux subsystem builds `futex` on it)
- [ ] Shared libraries for native programs (`libvexa.so` and Vexa's own dynamic loader)
- [ ] Linux: the musl dynamic loader, `clone`, `arch_prctl`, `set_tid_address`,
      `getrandom`, `/proc/self/maps`, `sched_getaffinity`, `prctl`, rlimits
- [ ] Linux: run Alpine Linux packages: bash, coreutils, python3

**Milestone:** multithreaded programs run on both tracks, including Python.

## Phase 7: Networking

- [ ] virtio-net (QEMU) and Intel e1000 drivers
- [ ] TCP/IP stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP; DHCP client (IPv6 later)
- [ ] Native socket API in the core; the Linux subsystem maps BSD sockets onto it
- [ ] Local sockets that can pass handles between processes (Linux: Unix domain
      sockets with `SCM_RIGHTS`). X11 and Firefox's multi-process IPC both depend on it
- [ ] DNS resolver in `libvexa`; Linux programs read `/etc/resolv.conf`

**Milestone:** a native Vexa program fetches a web page, and so does Linux `wget`.

## Phase 8: Graphics and input

Core:
- [ ] Mouse (PS/2), then USB: xHCI controller and HID keyboard/mouse
- [ ] Vexa display interface: find outputs, set modes, allocate and show buffers
      (boot framebuffer first, then a virtio-gpu driver in QEMU)
- [ ] Vexa input event interface for keyboards and mice
- [ ] Shared memory objects that can be mapped into several processes

Native track:
- [ ] A first Vexa compositor: windows as shared buffers, drawn on the screen,
      with mouse and keyboard focus
- [ ] A native terminal window running `vsh`

Linux track:
- [ ] Translate the Linux interfaces onto the core: DRM/KMS "dumb buffers",
      evdev devices under `/dev/input`, `memfd_create`, `/dev/shm`, `MAP_SHARED`
- [ ] Run an X server (Xorg with the modesetting driver), then `xterm`

**Milestone:** a graphical Vexa desktop with windows you can drag around.

## Phase 9: The desktop stack

- [ ] Linux: fontconfig and FreeType render text in X clients
- [ ] Linux: the GTK 3 demo (`gtk3-demo`) runs
- [ ] Linux: Mesa's software renderer (llvmpipe) for OpenGL, since Firefox's WebRender
      can fall back to software rendering anyway
- [ ] Run X11 windows inside the Vexa compositor (a small X server that draws into
      Vexa windows), so Linux apps share the screen with native ones
- [ ] Audio (optional for first light): Intel HDA driver, a native audio interface,
      and an ALSA-compatible layer for Linux programs

**Milestone:** GTK applications run next to native programs on the Vexa desktop.

## Phase 10: Firefox

- [ ] Launch with the content sandbox disabled (`MOZ_DISABLE_CONTENT_SANDBOX=1`),
      because seccomp-bpf and user namespaces can come much later
- [ ] Implement the Linux calls it hits that Vexa does not support yet (the log of
      unimplemented calls from Phase 5 becomes the to-do list)
- [ ] Multi-process: parent, content, GPU/RDD and socket processes all talking over
      Unix sockets and shared memory
- [ ] Performance: enough RAM (give QEMU 4 GiB or more), SMP, a decent page cache
- [ ] Load `https://www.mozilla.org` with working TLS

**Milestone: Firefox runs on Vexa.**

---

## Working habits that make this feasible

- **Test in QEMU on every change.** Add a CI job that boots the ISO headless and
  checks the serial log. `make run-nographic` is the starting point.
- **Log unknown Linux system calls** with their number and arguments instead of
  silently failing.
- **Keep the Linux subsystem building as an add-on.** Build and boot with
  `LINUX_COMPAT=0` in CI too, so the core never starts depending on it.
- **Use GDB with QEMU** (`-s -S`, then `target remote :1234`), since the kernel is built
  with `-g`.
- **Keep each phase shippable.** Finish a milestone before starting the next.
- **Run on real hardware** occasionally, starting after Phase 1. Emulators forgive a lot.

## References

- [ARCHITECTURE.md](ARCHITECTURE.md): how the core, native interface and Linux subsystem fit together
- [OSDev Wiki](https://wiki.osdev.org/): the starting point for almost every topic here
- [Intel 64 and IA-32 Software Developer's Manuals](https://www.intel.com/sdm): CPU reference
- [Limine boot protocol](https://github.com/limine-bootloader/limine-protocol)
- [Linux syscall table for x86_64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)
  and the `man 2` pages for exact semantics
- *Operating Systems: Three Easy Pieces* (free online): concepts behind Phases 2 to 5
