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

Everything but `personality/linux/` exists today, along with `fs/` (file systems) under
`kernel/src/`. IPC and networking arrive in later phases of the [roadmap](ROADMAP.md), as
do `vinit` and `vsh`.

## Handles and files today

A process refers to kernel objects through **handles**: small numbers in its handle
table, each with the rights it was opened with (read, write). Open files are the first
kind of object; pipes, processes and sockets will be others. The Linux subsystem will map
Unix file descriptors onto the same table.

Files live in one tree managed by the **VFS** (`core/vfs.c`). File systems plug in
underneath it:

- `tmpfs`: in memory; the root file system, filled from `initramfs.tar` at boot
- `devfs`: `/dev`, with `null`, `zero`, `console` and every disk and partition
- `ext2`: disks, mounted at `/mnt/<disk>`

Disks sit behind the block layer (`core/block.c`), which caches them in 4 KiB chunks,
reads partition tables, and calls the drivers (`dev/virtio_blk.c`, `dev/ahci.c`,
`dev/nvme.c`).

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
