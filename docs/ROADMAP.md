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

## Phase 1: Boot and CPU basics *(done)*

- [x] Boot with Limine on BIOS and UEFI, higher-half 64-bit kernel
- [x] Serial logging, `kprintf`, `panic`
- [x] GDT and IDT with CPU exception reporting
- [x] Read the memory map and framebuffer from the bootloader
- [x] Text console on the framebuffer (bitmap font, scrolling)
- [x] Parse ACPI tables (MADT) and set up the Local APIC and I/O APIC, with a legacy
      8259 PIC fallback for machines without a MADT
- [x] Timer (APIC timer calibrated against the PIT; HPET fallback later)
- [x] PS/2 keyboard driver

**Milestone:** type on the keyboard and see the characters on screen. Reached: the kernel
ends in a small built-in command line (the kernel monitor) that stays until `vsh` replaces it.

## Phase 2: Memory management *(done)*

- [x] Physical page allocator (buddy allocator, blocks of 4 KiB to 4 MiB)
- [x] Virtual memory manager: Vexa's own page tables with a direct map of RAM, the kernel
      mapped with W^X permissions (NX and read-only data), and the bootloader's memory
      reclaimed
- [x] Kernel heap: `kmalloc`/`kfree` on slab caches, whole pages for larger objects
- [x] Page fault handler that explains the fault; kernel stacks with guard pages, and a
      TSS with separate stacks for double faults, NMIs and machine checks, so a stack
      overflow is reported instead of resetting the machine
- [ ] Moved to later phases: locking for SMP (Phase 3) and demand paging for user
      memory (Phase 5, with `mmap`)

**Milestone:** allocate and free millions of objects without leaking or crashing. Reached:
the `memtest` command runs 5 million random allocator operations (over a million heap
allocations) and checks every byte and page comes back.

## Phase 3: Processes, threads and user mode *(done)*

- [x] Kernel threads and a preemptive round-robin scheduler driven by the timer, with
      sleeping, wait queues and an idle thread per CPU
- [x] TSS, ring 3 segments, jumping to user mode; a separate address space per process
- [x] One `syscall` entry point that hands each call to the calling process's
      personality (see [ARCHITECTURE.md](ARCHITECTURE.md))
- [x] The first native Vexa system calls: `vx_exit`, `vx_log`, `vx_yield`, `vx_sleep`,
      `vx_process_id`, `vx_uptime`
- [x] ELF64 loader that picks the personality from the executable's `.note.vexa` note
- [x] Save/restore FPU, SSE and AVX state (`XSAVE`, or `FXSAVE` on older CPUs)
- [x] SMP: start the other CPU cores (up to 64), with locks in the allocators and
      scheduler, x2APIC support, and a panic that halts every core
- [x] SMEP and SMAP where the CPU has them, so the kernel can't be tricked into running
      or reading user memory
- [x] `libvexa` begins: program startup, system call wrappers, `printf`, string functions
- [ ] Moved to later phases: per-CPU run queues and priorities (once there are
      workloads to measure), TLB shootdowns between CPUs (needed with multi-threaded
      processes and `munmap`, Phases 5-6)

**Milestone:** a native Vexa "hello world" runs in user mode. Reached: `run hello-world`.
`make test` also runs `crash` (a program that must be stopped without harming the
system) and three `fpu-stress` programs at once, which check that every program's vector
registers survive being interrupted.

## Phase 4: Files and storage *(done)*

- [x] Handle table: one kernel object model (files now; pipes, processes and more later)
- [x] VFS layer (vnodes, path lookup, mount points) and file system calls: `vx_open`,
      `vx_read`, `vx_write`, `vx_seek`, `vx_stat`, `vx_read_dir`, `vx_mkdir`, `vx_remove`
- [x] initramfs loaded as a Limine module (tar), unpacked into the root file system
- [x] tmpfs and a device file system (`/dev/null`, `/dev/zero`, `/dev/console`, disks)
- [x] PCI enumeration (ECAM or legacy ports), MSI and MSI-X interrupts
- [x] Block layer: write-through block cache, GPT and MBR partitions
- [x] virtio-blk driver (QEMU), then AHCI and NVMe for real hardware; each falls back to
      polling without MSI
- [x] ext2 read/write (ext4 later), so disks can be shared with other systems
- [ ] Moved to later phases: USB storage (with USB in Phase 8), ext4, a journal or
      another crash-safe file system, finer-grained VFS locking, running `vinit` from a
      disk as the root file system

**Milestone:** boot from a disk image and read files from it. Reached: disks are
mounted under `/mnt`, and `run /mnt/vda1/hello-world` runs a program from one. `make test`
writes to virtio, SATA and NVMe disks and then checks them with Linux's `e2fsck`.

## Phase 5: Vexa userland *(done)*

Native track:
- [x] `libvexa`: C startup code, strings, memory allocator, `printf`, buffered file I/O,
      environment variables, over native system calls
- [x] Native process calls: spawn, wait, exit, kill, signals, process groups; memory
      mapping; pipes; current directory
- [x] A terminal (`/dev/tty`): line editing, Ctrl-C, Ctrl-D, raw mode; ANSI escape
      sequences on the console
- [x] Demand paging and copy-on-write pages (the Linux `fork` needs them)
- [x] `vinit` (first process) and `vsh` (the Vexa shell: pipes, redirection, variables,
      background programs, scripts)
- [x] Core utilities written for Vexa: `ls`, `cat`, `echo`, `mkdir`, `rm`, `cp`, `mv`,
      `pwd`, `ps`, `kill`, `sleep`, `clear`, `uptime`, `hello`, `sys`

Linux track:
- [x] Create `kernel/src/personality/linux/` and the `LINUX_COMPAT` build option
      (`make test` also boots a kernel built without it)
- [x] Linux calls mapped onto the core: files and directories, `mmap` and `brk`,
      `fork` (copy-on-write), `execve`, `wait4`, process groups, signals with handlers
      (`rt_sigaction`, signal frames, `rt_sigreturn`, `SA_RESTART`), terminal `ioctl`s,
      `poll` and `select`, clocks and sleeping; about 150 calls
- [x] Log every unimplemented Linux call with its number and arguments
- [x] Run static BusyBox from the Vexa shell: built from a pinned release (1.36.1) with
      musl, installed as `/linux/bin/busybox`; its source ships with every release
- [ ] Moved to later phases: symbolic links, `/proc` (for `ps` and `top`), timers that
      send signals (`alarm`), non-blocking I/O

**Milestone:** a working Vexa shell, and Linux BusyBox running from it. Reached in
0.7.0: `busybox sh` runs from `vsh`, with pipes, job control, Ctrl-C, `trap` and `vi`.

## Phase 6: Dynamic linking and threads

- [x] Threads, thread-local storage, and a core "wait on address" primitive
      (native API first: `vx_thread_create`, `vx_wait_address`, `vx_mutex` in libvexa;
      the Linux subsystem builds `futex` on it). Several threads per process on every
      CPU, per-thread signals, exit of one thread or all, exec from a threaded process,
      and TLB shootdowns between CPUs (0.9.0)
- [ ] Shared libraries for native programs (`libvexa.so` and Vexa's own dynamic loader)
- [x] Linux: the musl dynamic loader (`PT_INTERP`, `AT_BASE`), and Linux programs'
      files under `/linux` (tried first, like FreeBSD's Linux emulation)
- [x] Symbolic links (tmpfs, ext2, initramfs, both system call interfaces), `#!`
      scripts, `/proc` (`self`, `<pid>/stat|status|cmdline|maps|exe|cwd|fd`, `meminfo`,
      `stat`, `cpuinfo`, `mounts`...) and `/dev/fd`
- [x] Linux: BusyBox linked dynamically; GNU bash 5.2 built from source
      (0.8.0: Alpine's package servers can't be reached from the build machines, so
      Linux software is built from source for now)
- [x] Linux: `clone` for threads (TLS, parent/child tid), `set_tid_address` clearing,
      `futex` (wait, wake, bitsets, requeue), `gettid`, `tgkill`: musl's pthreads work
      (0.9.0); robust futex lists are accepted but not acted on yet
- [ ] Linux: more software: coreutils, python3
- [ ] Moved here from Phase 5: timers that send signals (`alarm`), non-blocking I/O,
      file permissions; file-backed memory that's shared rather than copied

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
