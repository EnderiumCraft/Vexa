# Vexa architecture

Vexa is its own operating system with its own design. It can also run Linux programs
through a separate compatibility subsystem. This document describes how those two
things fit together, and the rules that keep Linux compatibility from shaping the rest
of the system.

## Layers

```
 ┌───────────────────────────────┐   ┌───────────────────────────────┐
 │  Native Vexa programs         │   │  Linux programs               │
 │  vinit, vsh, utilities,       │   │  BusyBox, Python, Xorg, GTK,  │
 │  compositor, ...              │   │  Firefox, ...                 │
 ├───────────────────────────────┤   ├───────────────────────────────┤
 │  libvexa  (Vexa's C library)  │   │  musl or glibc (unmodified)   │
 └───────────────┬───────────────┘   └───────────────┬───────────────┘
                 │ Vexa system calls                 │ Linux system calls
 ════════════════╪═══════════════════════════════════╪═══════════ user / kernel
 ┌───────────────┴───────────────┐   ┌───────────────┴───────────────┐
 │  Native personality           │   │  Linux personality (optional) │
 │  personality/vexa/            │   │  personality/linux/           │
 └───────────────┬───────────────┘   └───────────────┬───────────────┘
                 └─────────────────┬─────────────────┘
 ┌─────────────────────────────────┴─────────────────────────────────┐
 │  Vexa core                                                        │
 │  handles and objects · processes and threads · scheduler          │
 │  address spaces · VFS · IPC · networking · display and input      │
 ├───────────────────────────────────────────────────────────────────┤
 │  Drivers  (serial, framebuffer, APIC, PCI, storage, network, USB) │
 ├───────────────────────────────────────────────────────────────────┤
 │  Architecture support  (arch/x86_64: GDT, IDT, paging, syscall)   │
 └───────────────────────────────────────────────────────────────────┘
```

### Vexa core

The core is where the operating system actually lives. It is designed for Vexa only,
without regard to how Linux does things. Its central idea is the **handle**: every kernel
resource a program can hold (file, directory, pipe, socket, process, thread, shared
memory region, event) is an object, and a process refers to objects through handles.
All objects share one set of operations for waiting, duplicating, passing to another
process and closing.

### Personalities

A personality is the layer that turns system calls into core operations. Every process
has exactly one personality, chosen when its executable is loaded:

- An ELF file with a `.note.vexa` note section runs with the **native personality**.
  `libvexa`'s startup code adds this note to every Vexa program.
- Any other x86_64 ELF file runs with the **Linux personality**, if the kernel was
  built with it. Otherwise loading fails with "not an executable format".

There is one `syscall` entry point in `arch/x86_64`. It saves user registers and calls
the current process's personality dispatcher.

### Native interface

The Vexa system call interface is Vexa's own design: its own numbering, calls named
`vx_*`, handles instead of file descriptors, and a single wait call that can wait on any
set of objects. `libvexa` wraps it in C functions and provides the standard C library
on top, so ordinary C code still compiles for Vexa. The interface can change freely until
Vexa 1.0; only `libvexa` has to keep up.

Native programs are position-independent executables linked against `libvexa.so`.
Their `PT_INTERP` names Vexa's own dynamic loader, `/lib/vexa-ld.so` (`libvexa/ld/`):
the kernel maps the program and the loader, the loader relocates itself, reads the
libraries the program needs from `/lib`, binds symbols (the program's first), sets page
permissions with `vx_protect`, and jumps to the program. Each program still carries
`crt0` (its entry point and the `.note.vexa` note); `hello-world` stays statically
linked, so both kinds keep being tested.

### Linux subsystem

`personality/linux/` translates Linux x86_64 system calls into core operations. File
descriptors become handles, `futex` becomes the core's wait-on-address primitive,
`/proc` and `/dev` entries that Linux programs expect are provided by small file systems
inside the subsystem, and Linux `ioctl`s for terminals, DRM and input are translated onto
the core's own interfaces.

It is compiled in with `make LINUX_COMPAT=1` (the default once it exists) and left out
with `LINUX_COMPAT=0`.

## Rules

1. **The core never knows about Linux.** Nothing outside `personality/linux/` includes
   Linux headers, uses Linux system call numbers or refers to Linux structures.
2. **The Linux subsystem only calls core APIs.** It never reaches into scheduler,
   memory manager or driver internals.
3. **Missing features go into the core in general form.** When a Linux program needs
   something the core lacks, add a primitive that native programs can use too, and
   build the Linux behaviour on it. For example, `futex` is built on a general
   wait-on-address primitive and `SCM_RIGHTS` on handle passing.
4. **Native first when designing.** New kernel features get their native interface first.
   The Linux mapping comes second, and it must not force a worse native design.
5. **Vexa must work without Linux.** CI builds and boots a `LINUX_COMPAT=0` kernel,
   and the native userland must never depend on Linux binaries.

## Source layout

```
kernel/src/
  arch/x86_64/            CPU setup, interrupts, paging, syscall entry
  core/                   handles, processes, scheduler, memory, VFS, IPC, net
  dev/                    drivers
  lib/                    kernel support code (strings, kprintf)
  personality/vexa/       native system calls
  personality/linux/      Linux subsystem (LINUX_COMPAT)
abi/vexa/                 the native interface's numbers and constants, shared by
                          the kernel and libvexa
libvexa/                  Vexa's C library
userland/                 native programs: vinit, vsh, utilities, compositor
```

All of these exist today except the compositor, along with `fs/` (file systems) under
`kernel/src/`. Networking and more IPC arrive in later phases of the
[roadmap](ROADMAP.md).

## The Linux subsystem today

`personality/linux/` runs Linux x86_64 programs, static or dynamically linked (BusyBox
and bash so far). When the ELF loader finds no `.note.vexa` note in a program, the
process gets the Linux personality, and its system calls go to `linux_syscall()`, which
translates each one into core operations:

- **File descriptors are handle numbers.** `open` becomes `vfs_open` plus a handle;
  `dup2`, `fcntl` and close-on-exec work on the same handle table native programs use.
- **Processes.** `fork` is `process_fork` in the core (a copy-on-write address space
  and a copy of the handle table); `execve` is `process_exec`, which can also start a
  native program; `wait4` is `process_wait_child`.
- **Signals** use the core's numbering, which is Linux's. The core decides when a
  signal is delivered; the personality only builds the Linux signal frame on the user
  stack, and `rt_sigreturn` takes it down again. Vector registers are kept in the
  kernel while a handler runs, so a program can't hand back a malformed state.
- **Terminals.** The console terminal already uses Linux's termios flags, so
  `TCGETS`/`TCSETS` pass straight through.
- **Files live under `/linux`.** Linux programs expect `/lib/ld-musl-x86_64.so.1`,
  `/bin/sh`, `/usr/bin`... An absolute path from a Linux program is tried under `/linux`
  first and used as it is otherwise, so Vexa's own `/` stays Vexa's while `/dev`, `/tmp`,
  `/proc` and `/mnt` are shared. FreeBSD's Linux emulation works the same way. The core
  only knows this as an optional `translate_path` hook in the personality.
- **Dynamic linking.** The core's loader honours `PT_INTERP`: it loads the dynamic
  loader (musl's `libc.so`, at `/linux/lib/ld-musl-x86_64.so.1`) next to the program and
  starts it with `AT_BASE` set; the loader then maps libraries with `mmap`.

Anything missing returns `ENOSYS` and is logged once, with its arguments:
`[linux] prog (process 7): system call 41 is not implemented (...)`.

The kernel builds without the subsystem with `make LINUX_COMPAT=0`, and `make test`
boots such a kernel to check native Vexa never depends on it.

## Handles and files today

A process refers to kernel objects through **handles**: small numbers in its handle
table, each with the rights it was opened with (read, write). Open files, pipes,
processes and sockets are objects; an object can be non-blocking (Linux's
`O_NONBLOCK`), which every handle to it shares. The Linux subsystem maps Unix
file descriptors onto the same table.

Files live in one tree managed by the **VFS** (`core/vfs.c`). File systems plug in
underneath it:

- `tmpfs`: in memory; the root file system, filled from `initramfs.tar` at boot
- `devfs`: `/dev`, with `null`, `zero`, `console` and every disk and partition
- `ext2`: disks, mounted at `/mnt/<disk>`
- `iso9660`: CDs (read-only, with Rock Ridge names, permissions and links), mounted at
  `/mnt/cd0`...; the first with a `linux` directory, normally the boot CD, is also
  `/cdrom`. The Linux subsystem's programs and libraries live there: in the initramfs,
  `/linux` holds only what changes (`etc`, `var`, `root`) and links `bin`, `lib`,
  `sbin` and `usr` to `/cdrom/linux`, so they are read when used instead of taking
  memory from boot

Disks sit behind the block layer (`core/block.c`), which caches them in 4 KiB chunks,
reads partition tables, and calls the drivers (`dev/virtio_blk.c`, `dev/ahci.c`,
`dev/nvme.c`). AHCI drives CD/DVD drives too, through ATAPI (SCSI commands in a
PACKET command), as `cd0`... with 2048-byte sectors.

## Graphics and input today

- **Devices with state per open file.** `vnode_ops` has optional `open`, `close`,
  `file_read`, `file_write`, `file_poll` and `control` hooks, so a device can keep
  something for each open file (`file->private`). `vx_control(handle, request, arg,
  size)` is Vexa's `ioctl`: the argument is copied into the kernel and back.
- **Input** (`core/input.c`): drivers report events (Linux's types and codes, so the
  Linux subsystem's evdev view is a thin layer) and each open `/dev/input/eventN` gets
  its own queue. A program can grab a device; a grabbed keyboard no longer types into
  the console terminal. The PS/2 keyboard and mouse share the controller's interrupt
  path: each byte goes to one or the other by where it came from.
- **The display** (`dev/display.c`): `/dev/display0` is the boot framebuffer. A program
  acquires it, which hides the text console (it keeps its text and redraws when the
  program is done), and maps it with `vx_map_file`. The framebuffer's pages are device
  memory: page reference counts leave pages outside RAM alone.
- **Shared buffers.** `vx_map_file` maps a file shared (from any file system that can
  hand out its pages, like tmpfs); a window's pixels are a file in `/run/shm` that the
  program and the desktop both map.
- **Terminals** are `struct tty` instances: the console, and pseudo-terminals
  (`dev/pty.c`) whose master side is `/dev/ptmx` and whose terminal is `/dev/pts/N`,
  with the same line editing and job control signals.
- **The desktop** (`userland/desktop`) is an ordinary program: it grabs the keyboard and
  mouse, listens on the local socket `/run/desktop`, keeps windows in a stack, composes
  the parts of the screen that changed into memory and copies them to the display.
  It draws the panel (the Vexa menu, window buttons, a clock from `vx_time`), title
  bars with minimize, maximize and close buttons, and the outline of a window being
  resized. Programs use `<vexa/gui.h>`: `vx_window_create`, draw into
  `window->surface`, `vx_window_present`, and `vx_gui_wait` for key, pointer and close
  events. A window made with `VX_WINDOW_RESIZABLE` gets `VX_GUI_RESIZE` events and
  answers with `vx_window_resize`, which hands the desktop a new buffer.
- **X** runs as Linux programs. Xvexa (`third_party/xvexa`) is a kdrive X server
  built into X.Org's source tree, and it talks the desktop protocol itself, so the
  Linux subsystem needs nothing graphics-specific for it: local sockets, shared file
  mappings and `poll`. It is rootless: the Composite extension draws each top-level X
  window into a pixmap of its own, and Xvexa copies what X damaged there into that
  window's desktop window (menus and tooltips, override-redirect windows, are
  frameless popups). X's screen is as big as the real one, and X windows are kept
  where the desktop shows them (`DESKTOP_MOVED`), so X coordinates are screen
  coordinates; pointer events become X events at those coordinates, keys become X
  key events (Linux key codes, the `evdev` XKB rules), a desktop window's focus is
  X's input focus, a resize is a `ConfigureWindow`, and the close button sends
  `WM_DELETE_WINDOW`. Xvexa also stands in for a window manager: windows that draw
  their own title bar (`_MOTIF_WM_HINTS` without decorations, as GTK's client-side
  decorated windows) get undecorated desktop windows, and the requests programs send to
  the root window for a window manager (`_NET_WM_MOVERESIZE`, `_NET_WM_STATE`,
  `WM_CHANGE_STATE`, `_NET_ACTIVE_WINDOW`) become `DESKTOP_WM` messages, so GTK's own
  title bar buttons and dragging work. (Without `-rootless`, Xvexa shows its whole
  screen in one window.) `/linux/usr/bin/xrun <program>` starts it once and runs an X program;
  `xsession` (Ctrl+Alt+X) is `xrun xterm`. GTK 3 programs run the same way
  (the Vexa menu lists the programs in `/linux/usr/share/applications`), with
  `NO_AT_BRIDGE=1` (no accessibility bus yet). The X and GTK stack is built
  by `tools/build-x11.sh` with musl; C++ code that needs no C++ runtime (HarfBuzz) is
  compiled by the host g++ with musl's headers (`tools/musl-cxx-wrapper.sh`). A process's
  controlling terminal is what `/dev/tty` opens: a pty becomes one when a group
  leader opens it or claims it with `TIOCSCTTY`, and children inherit it.

## Networking today

`net/` is Vexa's own TCP/IP stack; `dev/virtio_net.c` is the one network card driver so
far.

- **One lock and one thread.** All protocol state is under `net_lock`, a sleeping
  mutex. A card's interrupt only wakes the network thread, which takes received frames
  from the drivers, passes them up (Ethernet, ARP, IPv4, then ICMP, UDP or TCP) and runs
  the timers: TCP retransmissions, ARP retries and DHCP. System calls take the lock for
  their part and never wait while holding it.
- **Interfaces.** `lo` (127.0.0.1), and a card per `ethN`, configured by DHCP. Routing
  is "the interface's own subnet, else its router". What the stack knows shows in
  `/proc/net/dev`, `route` and `resolv.conf`.
- **TCP** keeps 64 KiB send and receive buffers per connection, retransmits from the
  oldest unacknowledged byte with a doubling timeout, and drops segments that arrive
  out of order (the sender repeats them). No congestion control or window scaling yet.
- **Sockets** (`core/socket.h`) are objects shared by both personalities: `VX_AF_INET`
  (TCP, UDP, raw ICMP for `ping`) and `VX_AF_UNIX`. Addresses use Linux's layouts, so
  the Linux subsystem mostly handles calling conventions: iovecs, address lengths,
  control messages, options.
- **Local sockets** queue what is sent as chunks at the receiver. A chunk can carry
  objects (`SCM_RIGHTS`), each with a reference, so a descriptor sent by one process
  becomes a new handle in the other. A named socket's path is a file; connecting looks
  the socket up by that file.
- **Names.** `libvexa`'s `vx_resolve` reads `/etc/hosts`, then asks the name server
  DHCP gave us. Linux programs use musl's resolver and `/etc/resolv.conf`.
- **Waiting.** `poll`, `select` and `epoll` (and `vx_poll`) check objects' readiness
  every few milliseconds rather than being woken; good enough for now.

## Threads today

A process has a list of threads, all sharing its address space and handle table. Each
thread has its own kernel stack, saved registers, thread pointer (FS base), blocked
signal mask and pending signals; the process has pending signals too, which go to
whichever thread doesn't block them.

- **Ending.** A thread can end alone (`vx_thread_exit`, Linux `exit`), or end the whole
  process (`vx_exit`, `exit_group`): that marks the process as exiting and interrupts
  every thread, and each one leaves when it next heads back to user mode. The last
  thread to leave closes the handles; the process is over when the last one has been
  freed. `exec` in a threaded process first waits for the other threads to be gone.
- **Waiting on an address** (`core/futex.c`) is the one synchronization primitive:
  sleep while a 32-bit word holds a value, with an optional timeout; wake some or all
  of the sleepers on an address. libvexa's `vx_mutex` and joins, and Linux `futex`
  (so musl's mutexes, condition variables and `pthread_join`), are built on it.
- **TLB shootdowns.** Each address space knows which CPUs have it loaded. When a mapping
  is removed or made read-only, those CPUs get an interrupt and reload their page
  tables before the old pages can be reused; a CPU waiting for a spinlock answers
  such requests too, so shootdowns can't deadlock.

## How a system call works today

1. A program calls a `libvexa` function such as `vx_log(text, length)`, which puts the
   call number in `rax` and the arguments in `rdi`, `rsi`, ... and runs `syscall`.
2. `syscall_entry` (`arch/x86_64/syscall.S`) switches to the thread's kernel stack and
   saves the registers in the same frame layout interrupts use.
3. `syscall_dispatch` hands the frame to the process's personality, chosen by the ELF
   loader from the program's `.note.vexa` note. For Vexa programs that is
   `personality/vexa/syscalls.c`.
4. The handler checks every user pointer (`copy_from_user`), does the work through
   core functions, and stores the result in `rax`.
5. The frame is restored and `iretq` returns to the program.
