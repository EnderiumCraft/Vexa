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
  dev/                    drivers (dev/usb: xHCI, EHCI, UHCI, OHCI, the USB core, hubs, HID, storage)
  lib/                    kernel support code (strings, kprintf)
  personality/vexa/       native system calls
  personality/linux/      Linux subsystem (LINUX_COMPAT)
abi/vexa/                 the native interface's numbers and constants, shared by
                          the kernel and libvexa
libvexa/                  Vexa's C library
userland/                 native programs: vinit, vsh, utilities, compositor
apps/                     the desktop apps' bundles (Name.vxapp: Info.conf, icon)
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
underneath it. Each mount has a lock held around every call into its file system, so
a file system's code never runs twice at once and may sleep on disk I/O; operations on
names (lookups, creating, renaming, mounting) also hold `vfs_lock`, taken first, so the
tree stays still while a path is walked. Reading, writing and paging in a file take
only its file system's lock: a long read from the CD doesn't hold up `/tmp`.

- `tmpfs`: in memory; the root file system, filled from `initramfs.tar` at boot (or,
  started from an installed disk, only `/tmp` and `/run`)
- `devfs`: `/dev`, with `null`, `zero`, `console` and every disk and partition
- `ext2`: disks, mounted at `/mnt/<disk>`; ext3's journal too (`fs/journal.c`, JBD2):
  an operation's metadata changes are gathered in memory (`meta_read` and `meta_write`
  go through them), written to the journal with a commit block, then to their places,
  and the journal is marked empty; file data is written first. A journal holding
  committed transactions (Linux's too, with revoke records) is replayed at mount.
  ext4 disks are read through their extent trees (and 64-bit group descriptors) and
  mounted read-only
- `iso9660`: CDs (read-only, with Rock Ridge names, permissions and links), mounted at
  `/mnt/cd0`...; the first with a `linux` directory, normally the boot CD, is also
  `/cdrom`. The Linux subsystem's programs and libraries live there: in the initramfs,
  `/linux` holds only what changes (`etc`, `var`, `root`) and links `bin`, `lib`,
  `sbin` and `usr` to `/cdrom/linux`, so they are read when used instead of taking
  memory from boot

Disks sit behind the block layer (`core/block.c`), which caches them in 4 KiB chunks,
reads partition tables, and calls the drivers (`dev/virtio_blk.c`, `dev/ahci.c`,
`dev/nvme.c`, `dev/ata.c`). AHCI drives CD/DVD drives too, through ATAPI (SCSI commands
in a PACKET command), as `cd0`... with 2048-byte sectors; so does `ata.c`, the IDE driver
(PIIX and other controllers in IDE mode: disks `hda`... and CDs, by PIO), which
VirtualBox gives a new machine's CD drive.

**Root on a disk.** With `root=UUID=<uuid>` (or `root=vda2`) on the kernel's command
line, `init.c` looks for that ext2 file system among the disks (`storage_find_root`)
and mounts it at `/` instead of unpacking the initramfs, which then isn't needed. That's
how an installed system starts: `/bin/install` (`userland/install/`) writes a GPT
partition table, a FAT32 ESP and an ext2 file system with its own code (no Linux tools),
copies the running system into it, puts the kernel, Limine and a `limine.conf` with
`root=UUID=` on the ESP, and runs Limine's own `limine bios-install` (built for Vexa
from `limine/limine.c`, as `/bin/limine`) for BIOS booting; UEFI firmware finds
`EFI/BOOT/BOOTX64.EFI` by itself. The disk is asked about itself and told to read its
partition table again through controls on its `/dev` node (`VX_BLOCK_INFO`,
`VX_BLOCK_RESCAN`); a rescan mounts the new partitions. The Installer app runs
`/bin/install` and reads its steps (`step:`, `progress:`, `error:` lines) from a pipe.

**Users and permissions.** Each process has credentials (`struct cred`,
`<vexa/cred.h>`): real, effective and saved user and group ids and up to 16 other
groups. A spawned process gets its parent's effective ids as all three; a forked one
copies them; a program with the set-user-id (or set-group-id) bit runs as its file's
owner (or group): `apply_set_ids` in `core/process.c`, not for scripts. Kernel threads
are root. Vnodes have an owner and a group: ext2 keeps them in the inode (with Linux's
high 16 bits in `osd2`), tmpfs in memory, the initramfs gives the tar archive's
(root's), devfs makes devices `0666` and whole disks `0660` for group 10 (admin), and
FAT, exFAT and CDs have none (root's; FAT files `0777`). The VFS checks every
operation against the effective ids (`vnode_access`): search permission on each folder
a path goes through, read, write or execute to open, write and search on a folder to
make, remove or rename in it, the sticky bit's owner rule, and owner-only `chmod`;
`chown` is root's (or the owner's, to a group of their own). New nodes belong to
their maker, in the folder's group when it is set-group-id. `VX_SYS_CREDENTIALS`,
`VX_SYS_CHOWN`, `VX_SYS_CHMOD` and `VX_SYS_ACCESS` are the native calls; the Linux
subsystem's `getuid`/`setresuid`/`setgroups`/`chown`/`access`... use the same
credentials. Signals (and `vx_priority`) reach only one's own processes, unless one
is root.

The accounts themselves are files, read by libvexa (`<vexa/users.h>`): `/etc/passwd`,
`/etc/group` and `/etc/shadow` (`0600`, salted SHA-256 hashes). Set-user-id programs do
what needs root for others: `vauth` checks a password (the lock screen), `sudo`,
`accounts` (adding, removing, passwords), `install` and `hostname`, each checking who
asked. vinit (root) makes each account's home folder; the desktop starts as root,
logs someone in (by itself when there's one account without a password, otherwise
through the login screen, `desktop/lock.c`), then becomes them with
`vx_become_user`; logging out ends it with `DESKTOP_EXIT_LOGOUT`, and vinit starts it
again. Once an account has a password, vinit's console asks for a login too, and
starts that account's shell through `sudo -u`.

## Graphics and input today

- **Devices with state per open file.** `vnode_ops` has optional `open`, `close`,
  `file_read`, `file_write`, `file_poll` and `control` hooks, so a device can keep
  something for each open file (`file->private`). `vx_control(handle, request, arg,
  size)` is Vexa's `ioctl`: the argument is copied into the kernel and back.
- **Input** (`core/input.c`): drivers report events (Linux's types and codes, so the
  Linux subsystem's evdev view is a thin layer) and each open `/dev/input/eventN` gets
  its own queue. The first two, event0 and event1, are "all keyboards" and "all
  pointers": every keyboard's and pointer's events come out of them too, so the
  desktop reads those two and anything plugged in later just works. Tablets report
  absolute positions (`VX_EV_ABS`, 0 to `VX_ABS_MAX` across the screen). A program can
  grab a device; a grabbed keyboard no longer types into the console terminal. Every
  keyboard's keys go through one shared part (`core/keyboard.c`): input events, text
  for the console, and key repeat for keyboards that don't repeat by themselves (USB
  ones). An unplugged device stays as `/dev/input/eventN` (programs may have it open)
  and the next one of its kind takes its place. The PS/2 keyboard and mouse share the
  controller's interrupt path: each byte goes to one or the other by where it came
  from.
- **USB** (`dev/usb/`): `xhci.c` drives xHCI controllers (USB 1 to 3): a device
  context per slot, rings of TRBs for commands and each endpoint, and an event ring
  that a kernel thread per controller reads, woken by the MSI interrupt. Threads that
  start a command or a transfer sleep until its event; interrupt endpoints (keyboards,
  mice, hubs) stay queued, each report going to a callback. The older controllers
  (`ehci.c`, `uhci.c`, `ohci.c`, sharing `hcd.c`) keep a queue head (an ED, for OHCI)
  per endpoint, in the controller's asynchronous list (control, bulk) or its
  periodic one (interrupt endpoints, visited every frame), with a transfer's
  descriptors hung on it. They take 32-bit addresses, so their memory comes from below
  4 GiB (`pmm_alloc_below`) and data goes through bounce buffers there. They have no
  MSI: their interrupt is a legacy PCI one (INTx, below), which wakes the thread
  waiting for a transfer and a thread per controller that handles the interrupt
  endpoints and the ports. When it can't be routed, they check instead (every 2 ms
  with a keyboard or mouse, else every 20).
  EHCI starts before its companions (until then every port is theirs) and hands a
  port to them when what's on it isn't high speed; low and full speed devices behind
  a high speed hub get split transactions instead. UHCI keeps data toggles in
  software (`clear_toggle` after a halt is cleared). `usb.c` is the core: one
  kernel thread, "usb", handles plugging and unplugging, so drivers may sleep. A new
  device gets an address, its descriptors are read, it's configured, and each
  interface goes to the first class driver that takes it: `hub.c` (USB 2 and 3 hubs,
  whose ports are enumerated like the controller's), `hid.c` (boot-protocol
  keyboards; mice and tablets read with their report descriptors) and `storage.c`
  (bulk-only mass storage with SCSI commands: disks `usb0`, `usb1`..., mounted at
  `/mnt`). Unplugging takes a device away with everything under it; an unplugged disk's
  file systems are detached (`vfs_detach`) and its files still open fail with an I/O
  error.
- **The device tree** (`core/device.c`): every device the kernel found, for
  `vx_device_list` (Device Manager, `devices`): the processors, each PCI function
  (named by its class and vendor until a driver claims it with `pci_claim`), and what
  drivers make of them as children: disks (`block_register` adds them, with where
  they're mounted), input devices, USB devices as they come and go. Each has a kind,
  a bus, ids, a location and a line of details; a generation number changes with
  every change, so programs know when to look again.
- **Linux graphics and input** (`personality/linux/devices.c`): `/dev/dri/card0` is a
  DRM device with one CRTC, encoder and connector, and dumb buffers in memory that
  programs map; the buffer on screen is copied to the frame buffer when it's set,
  flipped to or marked dirty, and about 30 times a second besides. Showing one takes
  the display as `/dev/display0`'s holder would (`display_claim`); closing the card
  gives it back. Linux reads of `/dev/input/eventN` get `struct input_event`, and the
  `EVIOC*` requests describe the devices. Xorg runs on these with its modesetting
  driver (a shadow frame buffer in a dumb buffer) and evdev input driver, built from
  the same xorg-server source as Xvexa; `third_party/xorg` patches out its virtual
  console handling and stands in for the bit of udev evdev asks.
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
  It draws the panel (the Vexa menu, window buttons, search, a clock from `vx_time`
  with a calendar under it), title bars with round corners and minimize, maximize and
  close buttons, soft shadows, the outline of a window being resized, and animations
  (a window is drawn into a scratch surface and scaled, at about 60 frames a second
  while one runs). Its parts are in `userland/desktop/` (see the developer guide):
  Alt+Tab, search, the desktop's icons and the Desktop folder, screenshots (a PNG
  writer in libvexa), the screensaver and the lock screen. Programs use
  `<vexa/gui.h>`: `vx_window_create`, draw into
  `window->surface`, `vx_window_present`, and `vx_gui_wait` for key, pointer and close
  events. A window made with `VX_WINDOW_RESIZABLE` gets `VX_GUI_RESIZE` events and
  answers with `vx_window_resize`, which hands the desktop a new buffer.
- **Text** in Vexa's programs is TrueType: libvexa maps the DejaVu fonts in
  `/share/fonts` and rasterizes glyphs with stb_truetype (anti-aliased, kept per font
  and size in each program); strings are UTF-8. The desktop turns key codes into
  Unicode characters through the keyboard layout (with dead keys) and sends them with
  each key event.
- **Apps** are bundles, as on macOS: a folder `/apps/Name.vxapp` with
  `Contents/Info.conf` (name, executable, icon, the file types it opens, a shortcut,
  its place in the menu, whether it has a desktop icon), `Contents/Vexa/<program>`
  and `Contents/Resources/icon.png` (48x48 with transparency, drawn by
  `tools/make-app-icons.py`). `<vexa/app.h>` reads them (`vx_app_list`,
  `vx_app_find`, `vx_app_for_file`, `vx_app_open`); the desktop builds its menu,
  icons and shortcuts from them, Files shows a bundle as an app and opens files with
  the app for their type, and `open` does the same from the command line. The
  build moves each app's program into its bundle and leaves a link in `/bin`
  (`/bin/files`), so the command line still finds them. An app's executable may be
  an absolute path: `XTerm.vxapp` runs `/linux/usr/bin/xsession`, and is left out
  when the Linux files aren't there. The desktop looks at `/apps` every two seconds,
  so copying a bundle there installs an app and removing it uninstalls it.
- **Settings** are `key=value` files in `/etc` (`desktop.conf`, `apps.conf`, `hostname`),
  read and written through `<vexa/settings.h>`, which also copies them to
  `.vexa/etc` on the first writable ext2 disk; `vinit` copies them back at boot. The
  Settings app saves a change and sends `DESKTOP_RELOAD`; the desktop reads the file
  again, changes what changed (the wallpaper, the display's mode, key repeat, the
  menu), and sends every program `DESKTOP_THEME`, on which libvexa reads the theme
  again (`vx_theme`, dark or light with an accent color) and `vx_gui_wait` returns
  `VX_GUI_THEME` so the program draws itself again. Time zones (`<vexa/time.h>`) are a
  table of cities with their offsets and summer time rules.
- **The display** (`/dev/display0`) is the boot frame buffer; on QEMU's and Bochs's
  standard VGA (PCI 1234:1111) the holder can also set its mode (`VX_DISPLAY_MODES`,
  `VX_DISPLAY_SET_MODE`, through the card's DISPI registers), and the first mode comes
  back when it lets go. The desktop can also draw everything twice as big: it composes
  at half the size and doubles the pixels as it copies them out.
- **ACPI's namespace** (`core/aml.c`) comes from [uACPI](https://github.com/uACPI/uACPI)
  (`third_party/uacpi`, MIT), an AML interpreter: at boot it loads the DSDT and SSDTs
  and runs their initialization (`_INI`, `_STA`), with `\_PIC(1)` telling the firmware
  that interrupts go through I/O APICs. `aml.c` gives uACPI the services it asks for
  (mapping, PCI configuration space, ports, locks with timeouts, events, the SCI) and
  handles the power button. `noaml` on the kernel command line skips it.
- **Legacy PCI interrupts** (INTx): `pci_attach_interrupt` finds where a device's pin
  goes by the `_PRT` tables: the host bridge's, or the deepest bridge above the device
  that has one (pins rotate by slot through each bridge below it); an entry names an
  I/O APIC input or a link device, whose current resources give it. Without ACPI it
  falls back to the firmware's 8259 IRQ (the interrupt line register). Lines are
  shared (`irq_attach_line`): each handler says whether its device interrupted, and a
  line nobody claims for a long run is masked. The EHCI, UHCI and OHCI drivers and
  IDE channels in native mode use it.
- **Power** (`kernel/src/core/power.c`): turning off enters ACPI's S5 state (the PM1
  control registers from the FADT, the sleep type from the DSDT's `\_S5_` package,
  found by its name), with the ports virtual machines use as a fallback; restarting
  pulses the reset line. Native programs call `vx_power`; Linux programs `reboot()`.
- **Files** keeps its pictures (kinds of files, places) in its own bundle
  (`Files.vxapp/Contents/Resources`), makes thumbnails of pictures one at a time when
  it has nothing else to do, and works on whole folders with `<vexa/files.h>` (`vx_copy_tree`,
  `vx_remove_tree`, `vx_move`, which copies and removes across file systems,
  `vx_tree_size`, `vx_unique_name`); its clipboard is a file (`/tmp/.files-clipboard-UID`, each account's own)
  so every Files window shares it, deleting moves things to the account's Trash (`$HOME/.Trash`), and its
  right-click menus, like the desktop's, are `vx_draw_menu` (`<vexa/gui.h>`).
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

`net/` is Vexa's own TCP/IP stack. The network card drivers: `dev/virtio_net.c`,
`dev/e1000.c` (Intel e1000 and e1000e: legacy descriptor rings, MSI where the card has
it) and `dev/realtek.c` (RTL8139, with its single receive ring buffer, and
RTL8111/8168, with descriptor rings). Each registers with `net_register` under the next
`ethN` name.

- **One lock and one thread.** All protocol state is under `net_lock`, a sleeping
  mutex. A card's interrupt only wakes the network thread, which takes received frames
  from the drivers, passes them up (Ethernet, ARP, IPv4 or IPv6, then ICMP, ICMPv6, UDP
  or TCP) and runs the timers: TCP retransmissions, neighbour retries, DHCP and IPv6
  address autoconfiguration. System calls take the lock for
  their part and never wait while holding it.
- **Interfaces.** `lo` (127.0.0.1 and ::1), and a card per `ethN`, configured by DHCP.
  Routing is "the interface's own subnet, else its router". What the stack knows shows
  in `/proc/net/dev`, `route`, `if_inet6`, `ipv6_route` and `resolv.conf`.
- **IPv6** (`net/ipv6.c`). Each card gets a link-local address (fe80:: and its MAC
  address as an EUI-64), checks nobody else has it (duplicate address detection), then
  sends router solicitations; a router advertisement gives it an address in each /64
  prefix marked for autoconfiguration, a default route, and a DNS server if one is
  advertised. Neighbour discovery shares the neighbour table with ARP (`core.c` keys it
  by 16-byte address), so frames wait for either kind of answer the same way. No
  fragments, no multicast listener reports (the cards take every multicast frame), and
  link-local destinations go out of the first card (there are no zone ids yet).
- **Addresses above IP** are all 16 bytes (`ip6_t`): IPv6 addresses as they are, IPv4
  ones mapped (`::ffff:a.b.c.d`). UDP and TCP don't care which version a packet came
  by; `net_send` and `net_transport_checksum` pick it from the address. So a
  `VX_AF_INET6` socket bound to `::` takes IPv4 connections too, unless it's set
  IPv6-only (`IPV6_V6ONLY`, in the Linux subsystem).
- **TCP** keeps 64 KiB send and receive buffers per connection, retransmits from the
  oldest unacknowledged byte with a doubling timeout, and drops segments that arrive
  out of order (the sender repeats them). No congestion control or window scaling yet.
- **Sockets** (`core/socket.h`) are objects shared by both personalities: `VX_AF_INET`
  and `VX_AF_INET6` (TCP, UDP, raw ICMP and ICMPv6 for `ping`) and `VX_AF_UNIX`. Addresses use Linux's layouts, so
  the Linux subsystem mostly handles calling conventions: iovecs, address lengths,
  control messages, options.
- **Local sockets** queue what is sent as chunks at the receiver. A chunk can carry
  objects (`SCM_RIGHTS`), each with a reference, so a descriptor sent by one process
  becomes a new handle in the other. A named socket's path is a file; connecting looks
  the socket up by that file.
- **Names.** `libvexa`'s `vx_resolve` (A records) and `vx_resolve6` (AAAA) read
  `/etc/hosts`, then ask the name server DHCP gave us (or a router advertised).
  `getaddrinfo` gives IPv4 answers first and looks for IPv6 ones when asked to, or when
  a name has no IPv4 address. Linux programs use their own resolver and
  `/etc/resolv.conf`.
- **Waiting.** `poll`, `select` and `epoll` (and `vx_poll`) check objects' readiness
  every few milliseconds rather than being woken; good enough for now.

## Threads today

**Scheduling** (`core/sched.c`). Each CPU has a run queue with three priority bands,
from the thread's nice value (its process's, -20 to 19: below 0 high, 0 normal, above
0 low; kernel threads are normal). A CPU runs the first thread of its highest non-empty
band, but one that has waited over 100 ms goes first whatever its band, so nothing
starves; a CPU with nothing to do takes work from the busiest one. A woken thread goes
back to the CPU it ran on last, or to an idle CPU (woken by an IPI) if that one is
busy, and it preempts a lower-band thread at once. Within a band it's round robin with
10 ms slices; a thread that yields (polling for something) goes behind every band. One lock still covers the queues, wait queues and sleepers: it's held
across a context switch, so no CPU picks up a thread whose stack is still in use.

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
