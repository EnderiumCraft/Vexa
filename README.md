# Vexa

<p align="center"><img src="docs/logo.png" alt="Vexa OS" width="360"></p>

Vexa is a hobby operating system for x86_64, written from scratch in C. Why? Because funny.
The long-term goal is to run **Mozilla Firefox**.

Vexa has its own kernel design, its own system call interface and its own C library.
Linux programs such as Firefox run through a separate, optional compatibility subsystem
that sits on top of the Vexa kernel. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

**Documentation**: the [user guide](docs/USER-GUIDE.md) (using Vexa), the
[developer guide](docs/DEVELOPER-GUIDE.md) (building it, and writing programs and apps
for it), the [SDK](docs/SDK.md) (building native apps, and porting them with POSIX and
SDL 2, on another machine), the [API reference](docs/API.md) (libvexa), the
[architecture](docs/ARCHITECTURE.md) and the [roadmap](docs/ROADMAP.md).

![Vexa running in QEMU](docs/screenshot.png)

![The Vexa desktop: Music playing, with the glass title bars and panel](docs/desktop-screenshot.png)

## Status

Phases 1 to 7 are complete: boot and CPU basics, memory management, processes and
user mode, files and storage, a userland, threads and dynamic linking, and networking.
Vexa boots into its own shell, `vsh`, with a set of native programs, and runs Linux
programs (BusyBox, GNU bash and coreutils, Python 3.12) through the Linux subsystem.
Phase 8, graphics, is under way: Vexa has its own desktop with terminal windows, and
runs X programs such as `xterm` in windows of their own on it. The kernel:

- boots through the [Limine](https://github.com/limine-bootloader/limine) bootloader (BIOS and UEFI)
- runs in 64-bit long mode as a higher-half kernel
- shows a text console on the screen (with ANSI colors and cursor movement) and mirrors
  it to the COM1 serial port
- loads its own GDT and IDT and reports CPU exceptions with a register dump
- reads the ACPI tables and sets up the local APIC and I/O APIC, falling back to
  the legacy 8259 PIC on machines without them
- runs a 1000 Hz timer: the local APIC timer calibrated against the PIT, or the PIT
  itself
- manages memory with a buddy page allocator, its own page tables (read-only code,
  no-execute data) and a slab-based kernel heap (`kmalloc`/`kfree`)
- gives programs memory on demand, and shares pages copy-on-write
- runs on kernel stacks with guard pages, and reports stack overflows and other faults
  in plain words
- reads PS/2 and USB keyboards and mice (with scroll wheels) and tablets, as input
  events programs can read (the text console types a US layout; the desktop, the layout Settings chose)
- has user accounts: files with owners and Unix permissions, checked by the kernel;
  root and administrators; a login screen (and a console login) once an account has
  a password, Log Out, the lock screen asking for the account's password, Settings →
  Users to add and remove accounts, `sudo`, `accounts`, `id`, `chmod` and `chown`;
  each account with its own home folder and settings
- boots straight to its desktop (from the CD: the Installer first), with a glossy look
  in the spirit of Aqua and Aero: glass title bars and panel that blur what's behind
  them, glossy rounded window buttons in the accent's colors, gel-like buttons, switches and menus in
  Vexa's apps, light by default with a teal accent (a dark theme in neutral greys, an
  automatic one that's dark at night, and other accents in Settings)
- has a graphical desktop: a compositor that draws programs' windows (shared buffers)
  on the screen, with soft shadows, round corners and animations, a panel (the Vexa
  menu, a button per window, search, the volume, a clock with a calendar and the
  notifications),
  desktop icons (apps, the Desktop folder's files, the Trash), notifications, windows
  you move, resize, maximize, minimize and snap to a half or a quarter (by dragging, or
  Super+arrows), Alt+Tab with pictures of the windows, search (Ctrl+Space: apps,
  settings, files, sums), screenshots (PrintScreen), a screensaver and a lock screen,
  and its own apps: a terminal with tabs, Files, a text editor with tabs, undo and
  syntax colouring, an image viewer (PNG, BMP, PPM), Settings, Activity Monitor, a
  calculator, a calendar, Notes, Paint, Music (MP3, Ogg, FLAC, WAV), Videos (MPEG-1) and Help, sharing Open and Save dialogs and
  one clipboard (with X programs too); the default wallpaper (`/share/pictures/glass.png`) comes in the colors of
  every theme and accent, and changes with them
- draws text smooth with TrueType fonts (DejaVu, through stb_truetype) in UTF-8, so
  Vexa's apps show and type any language's letters, with whole keyboard layouts
  (German, French, Spanish, UK, Dvorak: AltGr and accent keys)
- keeps its desktop apps as bundles, as macOS does: `/apps/Files.vxapp` is a folder
  with the program, its icon and an `Info.conf` saying what it is and which files it
  opens; the menu, the desktop icons, Files and `open` all work from them, and
  copying a `.vxapp` into `/apps` installs it (the desktop notices)
- manages files like Finder: a sidebar (places and disks), Back and Forward, a list
  with columns you sort by or icons with picture thumbnails, selecting several
  things (Ctrl-click, Shift-click, Ctrl+A) and dragging them onto a folder (Ctrl
  copies), Quick Look (Space), search (Ctrl+F) and typing a name to go to it; copy,
  cut and paste (Ctrl+C, X, V), Duplicate (Ctrl+D), Make Alias, rename (F2), New
  Folder and New Text Document, Move to Trash (Delete) and Empty Trash,
  Get Info (Ctrl+I), Show Package Contents; right-click menus in Files and on the
  desktop
- has System Settings like macOS's: dark and light themes with an accent color that
  every window follows at once, wallpapers (fill, fit, center, tile), the panel and
  clock, time zones by city with summer time, mouse speed, double click, natural
  scrolling and left-handed buttons, keyboard layouts and key repeat, the display's
  resolution (on QEMU's standard VGA) and a 2x scale, default apps, startup apps, the
  computer's name, network and storage details; settings are kept on a disk when there
  is one, and restored at boot; Restart and Shut Down (ACPI)
- runs the X Window System through the Linux subsystem: Xvexa, an X server (X.Org's,
  built with musl) that shows each X window as a desktop window of its own, with
  TrueType fonts (FreeType, fontconfig, Xft and DejaVu), `xterm`, and GTK 3 programs
  (GLib, cairo, Pango and HarfBuzz, gdk-pixbuf): `gtk3-demo` is in the Vexa menu
- has a terminal with line editing, Ctrl-C (or Ctrl-\\) to stop programs and Ctrl-D for
  end of input
- runs threads with a preemptive scheduler, on every CPU core it finds; a program can
  have many threads, which synchronize by waiting on memory addresses (futexes)
- runs programs in user mode, each in its own address space, and stops a program
  that misbehaves without taking the system down
- has processes with parents and children, process groups, pipes and signals
- saves and restores each program's floating point and vector registers (SSE, AVX)
- has its own system call interface and C library, `libvexa`, with much of POSIX
  (files, directories, pthreads, time, `mmap`, `poll`, the whole of `stdio` and musl's
  libm) for programs ported to it
- has an SDK for building native programs and apps on Linux: `vexa-cc` and `vexa-c++`
  (with libc++), an app template (`vexa-new-app`), a CMake toolchain file, SDL 2 with
  Vexa drivers (each SDL window a desktop window, OpenGL through Mesa's softpipe or,
  in QEMU, on the host's GPU through the virtio GPU with 3D (virgl),
  sound on the HD Audio device), and SDL_mixer; each release carries it
- plays Doom: Chocolate Doom, built with the SDK like any SDL program, with Freedoom's
  levels, art and music
- browses the web with NetSurf (HTML, CSS, images, HTTPS, a little JavaScript), a
  native app built with the SDK
- has a file system tree with a root in memory (unpacked from an initramfs), `/dev`,
  and disks mounted under `/mnt`
- installs itself on a disk (the Installer app, or `install`) and starts from it, with
  BIOS or UEFI firmware, keeping what you change; the first login then opens a Welcome:
  the look, keyboard and time zone, the network, and a short tour
- drives USB: xHCI controllers (USB 1 to 3) and the older EHCI, UHCI and OHCI ones,
  hubs, keyboards, mice and tablets, and
  USB sticks and disks (mounted at `/mnt/usb0`), plugged in and out while it runs
- drives wired network cards: virtio-net, Intel e1000 and e1000e, Realtek RTL8139 and
  RTL8111/8168, AMD PCnet (so VirtualBox's adapters all work)
- drives QEMU's virtio GPU with 3D (virgl), for OpenGL on the host's graphics card,
  with Linux's virtgpu DRM interface (`/dev/dri/renderD128`)
- keeps a tree of every device and its driver, which Device Manager (an app) and
  `devices` (a command) show
- drives disks through virtio-blk (virtual machines), AHCI (SATA disks and CD/DVD
  drives), NVMe and IDE (older PCs, and VirtualBox's CD drive), reads GPT and MBR partition tables, reads and writes ext2 file
  systems, and reads CDs (ISO 9660 with Rock Ridge): the Linux programs and libraries
  are read from the boot CD when they're used, not loaded into memory at boot
- has symbolic links, `#!` scripts, and `/proc` (in Linux's format, so `ps` and `top`
  work)
- is on the network: a virtio-net driver, its own TCP/IP stack (Ethernet, ARP, IPv4,
  ICMP, UDP, TCP) with a DHCP client and a loopback interface, and sockets for both
  kinds of programs, including local (Unix domain) sockets that can pass open files
  between processes
- runs Linux programs through an optional Linux subsystem, static or dynamically linked
  (with musl's loader): `fork`, `execve`, signal handlers, terminal control and about
  190 system calls in all. Linux programs find their files under `/linux` first.

At boot, `vinit` (the first program) starts `vsh`, the Vexa shell. Some things to try at
the `vexa:/>` prompt:

| Command | What it does |
| --- | --- |
| `hello` | says hi, and which version of Vexa is running |
| `help` | how to use the shell; `ls /bin` lists the programs |
| `ls /`, `cat /etc/motd`, `cd /tmp`, `pwd` | look around the file system |
| `echo hi > note.txt`, `mkdir`, `cp`, `mv`, `rm -r` | make, copy, move and remove files |
| `ls /bin \| cat`, `cat < note.txt`, `echo more >> note.txt` | pipes and redirection |
| `hello-world` | the first Vexa program |
| `crash` | a program that misbehaves on purpose, to show it gets stopped |
| `fs-test` | checks the file system calls |
| `fpu-stress &` | runs a program in the background (try it three times, then `ps`) |
| `sleep 30`, then Ctrl-C | stops the program in front |
| `ps`, `kill`, `uptime` | processes and how long the system has been up |
| `nice -n 10 fpu-stress &` | runs a program at a lower priority (`renice` changes it later) |
| `thread-test`, `pthread-test` | threads: a Vexa program, and a Linux one using musl's pthreads |
| `posix-test` | checks libvexa's POSIX layer: files, `printf` and `scanf` with floats, libm, pthreads, time |
| `devices`, `devices -l usb` | the devices Vexa found and their drivers, as a tree; only the USB ones, in detail |
| `install --list`, `install vda` | the disks Vexa can be installed on; install it on one (erasing it) |
| `sys` | the kernel's own commands: `sys disks`, `sys mount`, `sys pci`, `sys cpu`, `sys mem`, `sys memtest`, `sys threads` |
| `bash` | GNU bash, a Linux program (`exit` to go back); inside it, `ls`, `vi`, `grep`, `ps`, `top`... are BusyBox's |
| `sh`, `busybox` | BusyBox's shell; `busybox` alone lists its commands |
| `ln -s`, `cat /proc/meminfo` | symbolic links; `/proc` |
| `desktop` | the graphical desktop, with a terminal window; the Vexa menu (top left) and the icons on the left start programs, Alt+Tab switches windows (with pictures of them), dragging a window to an edge or corner snaps it (so do Super+arrows), Ctrl+Space searches, PrintScreen takes a screenshot, Super+L locks the screen, Ctrl+Alt+T opens a terminal, Ctrl+Alt+F Files, Ctrl+Alt+E the text editor, Ctrl+Alt+X an `xterm` (an X program), Ctrl+Alt+Q goes back to the text console |
| `files`, `edit <file>`, `view <image>`, `settings` | the desktop's apps, from its terminal: `view /share/pictures/aurora.png`; in Files, a right click shows what you can do (copy, rename, Get Info, Move to Trash...) |
| `open <file>`, `open -a <app>` | opens a file with its app, a folder in Files, or starts an app, like macOS's `open`: `open /share/pictures/aurora.png`, `open -a Editor notes.txt` |
| `notify <text>` | a notification on the desktop |
| `hostname`, `df`, `shutdown [-r]` | the computer's name, how full the disks are, turning off or restarting |
| `input` | the keyboard and mouse; `input watch 1` shows what the mouse reports |
| `net` | network interfaces and addresses (Linux: `ifconfig`, `route -n`) |
| `fetch https://example.com/` | downloads a web page, over HTTP or HTTPS (a Vexa program); `wget` is BusyBox's |
| `socket-test`, `bsd-socket-test` | checks sockets: a Vexa program and a Linux one |

The kernel's built-in command line (the kernel monitor) is still there for when
something is broken: pick it in the boot menu, and Vexa starts it instead of `vinit`.

If Vexa has trouble on a machine, pick **safe mode** in the boot menu. It ignores ACPI
and uses only the oldest, most widely supported interrupt and timer hardware. The
kernel options behind it (`acpi=off`, `noapic`) can also be set in `limine.conf`, as can
`nosmp` to use only the first CPU core and `noaml` to skip running ACPI's code.

### Network

`make run` gives Vexa a sound card (HD Audio, played through PulseAudio or Core Audio;
`make run QEMU_AUDIO=` leaves it out) and a virtio network card behind QEMU's user-mode NAT: DHCP hands out
10.0.2.15, the router is 10.0.2.2 (which is also your machine), and DNS goes through
10.0.2.3. Try `net`, `fetch http://example.com/`, or `wget -O - http://example.com/`.
Linux programs have HTTPS: `curl https://example.com/`, `openssl`, and Python's `ssl`,
with Mozilla's root certificates. IPv6 works too: QEMU's router advertises `fec0::/64`,
so Vexa also gets `fec0::5054:ff:fe12:3456` (try `ping6 fec0::2`).

### Disks

Vexa mounts every ext2 file system and CD it finds at `/mnt/<disk>`, such as `/mnt/vda1`
or `/mnt/cd0` (the boot CD is also `/cdrom`). The easiest way to try it is `make run-disk`: it boots with a small
disk (kept in `build/my-disk.img`, so what you write survives restarts). To prepare
your own disk image on Linux:

```sh
truncate -s 64M disk.img && mke2fs -t ext2 disk.img
qemu-system-x86_64 -M q35 -m 512M -cdrom build/vexa.iso -boot d \
    -drive file=disk.img,if=virtio,format=raw
```

ext3 disks (`mke2fs -t ext3`) have a journal, which Vexa keeps: pulling the plug
mid-write can't leave them inconsistent. ext4 disks (`mkfs.ext4`) are read, mounted
read-only: Vexa follows their extent trees but doesn't write them yet. USB sticks
and other systems' disks in FAT (12, 16, 32) or exFAT are read and written, long
names and all; `eject usb0` before pulling one out.

Phase 8 is under way: Vexa has a graphical desktop of its own (`desktop`), with terminal
windows you can drag, resize, maximize and minimize, and X programs run on it as windows
of their own: Ctrl+Alt+X opens an `xterm`.
Linux programs also get the kernel's display and input interfaces, DRM with "dumb
buffers" on `/dev/dri/card0` and evdev on `/dev/input`, so they can draw on the whole
screen without X (or run Xorg there: `startxorg`), OpenGL through Mesa's llvmpipe (in X windows, or into memory), a D-Bus
session bus, and sound: HD Audio, AC'97 and USB sound cards, programs playing at once (mixed, at
the panel's volume), `play`, and ALSA for Linux programs (`aplay`). See [docs/ROADMAP.md](docs/ROADMAP.md) for the
full plan from here to Firefox.

## Download

Every push to the default branch is built and boot-tested by GitHub Actions, and
published on the [Releases page](https://github.com/EnderiumCraft/Vexa/releases):

- **[Nightly](https://github.com/EnderiumCraft/Vexa/releases/tag/nightly)**: the newest
  ISO as soon as it's built, before the tests (it may not work), replaced on every push
- **[Latest build](https://github.com/EnderiumCraft/Vexa/releases/tag/latest-build)**:
  the newest ISO that passed the tests, replaced on every push
- **Versioned releases** (`v0.1.1`, ...): created whenever `VEXA_VERSION` in
  `kernel/include/vexa/version.h` changes, and kept permanently

Run a downloaded ISO with `qemu-system-x86_64 -M q35 -m 512M -cdrom vexa-<version>.iso`.

## Building

You need a Linux host (or WSL) with:

| Tool | Debian/Ubuntu package |
| --- | --- |
| GCC or Clang targeting x86_64 | `build-essential` |
| GNU ld | `binutils` |
| xorriso | `xorriso` |
| QEMU | `qemu-system-x86` |
| git, make | `git`, `make` |
| Python 3 (for `make test`) | `python3` |
| UEFI firmware (for `make test`) | `ovmf` |
| mke2fs and e2fsck (for test disks) | `e2fsprogs` |
| musl C compiler and Linux headers (for BusyBox and bash) | `musl-tools`, `linux-libc-dev` |
| curl (downloads bash's source once) | `curl` |
| Meson, Ninja, pkg-config, bison and gperf (for X) | `meson`, `ninja-build`, `pkg-config`, `bison`, `gperf` |
| GLib's code generators (for GTK) | `libglib2.0-dev-bin`, `gtk-update-icon-cache` |

```sh
make                # builds build/vexa.iso (fetches Limine on first run)
make run            # boots in QEMU; kernel log appears in your terminal
make LINUX_COMPAT=0 # a kernel without the Linux subsystem (and without /linux)
make run-disk       # the same, with a disk mounted at /mnt/vda1
make run-nographic  # headless boot, serial only (Ctrl-A then X to quit)
make test           # boots in QEMU (BIOS; UEFI with 4 CPUs and 6 GiB; safe mode), with
                    # virtio, SATA and NVMe test disks; types commands into the virtual
                    # keyboard, checks the replies, then checks the disks with e2fsck;
                    # and a kernel built without the Linux subsystem: all four at
                    # once, with KVM when /dev/kvm is there
make test-quick ONLY=desktop  # one boot, some of the checks (shell, network, desktop,
                    # linux, x, linux-net, disks; several with commas)
make clean
```

## Layout

```
Makefile             build, ISO creation, QEMU and test targets
.github/workflows/   CI: build, boot-test and publish releases
limine.conf          bootloader menu entry
kernel/
  linker.ld          higher-half kernel link script
  include/limine.h   Limine boot protocol header (0BSD, vendored)
  include/vexa/      kernel headers
  src/kmain.c        kernel entry point and boot sequence
  src/arch/x86_64/   per-CPU setup, interrupts, APIC and 8259 PIC, timer, FPU state,
                     system call entry, context switch, starting other CPUs
  src/net/           network stack: interfaces, ARP, IPv4, ICMP, UDP, TCP, DHCP, sockets,
                     local sockets
  src/core/          memory (pmm, vmm, address spaces, heap), scheduler, processes,
                     signals, pipes, handles, VFS,
                     block cache and partitions, ELF loader, ACPI, init, kernel monitor
  src/personality/vexa/  the native Vexa system calls
  src/personality/linux/ the Linux subsystem (optional: LINUX_COMPAT)
  src/dev/           serial, framebuffer and /dev/display0, text console, terminals and
                     pseudo-terminals, font, PS/2 keyboard and mouse, clock, PCI,
                     virtio-blk, virtio-net, AHCI, NVMe
  src/lib/           string functions, kprintf, panic
  src/fs/            ext2, ISO 9660, tmpfs, devfs, procfs, initramfs unpacking
abi/vexa/abi.h       system call numbers and error codes, shared by kernel and libvexa
rootfs/              files for the root file system (packed into initramfs.tar)
third_party/         BusyBox's build configuration, the X sources list (x11-sources.txt),
                     Xvexa, Vexa's X server (xvexa/), uACPI and stb; other sources are
                     fetched here at build time
libvexa/             Vexa's C library: program startup, system calls, printf, strings,
                     threads, networking, drawing and windows; built as libvexa.so, with
                     the dynamic loader in libvexa/ld/
userland/            Vexa programs, one directory each: vinit, vsh, ls, cat, ...
apps/                the desktop apps' bundles (Name.vxapp: Info.conf and icons)
tests/disk-content/  files put on the test disks
tools/
  bdf2c.py           converts a BDF bitmap font into the console font table
  qemu-smoke-test.py boot test used by `make test`
  make-disk.py       builds disk images (GPT, MBR or none) holding an ext2 file system
  configure-busybox.sh  applies third_party/busybox.config to BusyBox's configuration
  make-linux-root.sh builds the /linux tree: musl's loader, BusyBox and its links, bash
  build-x11.sh       builds the X libraries, Xvexa, xkbcomp and xterm with musl
  linux-files/       files for the /linux tree (xsession: what Ctrl+Alt+X runs)
docs/
  USER-GUIDE.md      using Vexa
  DEVELOPER-GUIDE.md building Vexa, and writing programs and apps for it
  API.md             libvexa's functions
  ARCHITECTURE.md    how the kernel, native interface and Linux subsystem fit together
  ROADMAP.md         the plan, phase by phase
```

To add a program, create `userland/<name>/main.c`: the Makefile builds every
directory there with libvexa and puts it in `/bin`, and typing `<name>` in the shell
starts it. To make it a desktop app, give it a bundle in `apps/` (see the
[developer guide](docs/DEVELOPER-GUIDE.md#app-bundles)).

The console font is [Spleen](https://github.com/fcambus/spleen) 8x16 by Frederic Cambus
(BSD 2-Clause license, reproduced in `kernel/src/dev/font.c`). Vexa's apps draw text
with the [DejaVu](https://dejavu-fonts.github.io) fonts (Bitstream Vera license and
public domain changes, `rootfs/share/fonts/LICENSE`), rasterized by
[stb_truetype](https://github.com/nothings/stb) (public domain or MIT,
`third_party/stb/stb_truetype.h`). Music decodes with
[minimp3](https://github.com/lieff/minimp3) (CC0),
[stb_vorbis](https://github.com/nothings/stb) (public domain or MIT) and
[dr_flac](https://github.com/mackron/dr_libs) (public domain or MIT-0), and Videos with
[pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT), all in `third_party/media`; the
sample music and video in `/share` were made for Vexa (`tools/make-media.py`) and are
CC0. The kernel interprets ACPI's AML with
[uACPI](https://github.com/uACPI/uACPI) 6.1.1 (MIT license, `third_party/uacpi`).

The ISO includes [BusyBox](https://busybox.net) 1.36.1 (GPL-2.0), built unmodified from
its source with the configuration in `third_party/busybox.config`, and
[GNU bash](https://www.gnu.org/software/bash/) 5.2.37 (GPL-3.0), built unmodified with
the options in the Makefile. [GNU coreutils](https://www.gnu.org/software/coreutils/) 9.4 (GPL-3.0) and
[Python](https://www.python.org) 3.12.3 (PSF License, with [zlib](https://zlib.net) 1.3
and [libffi](https://sourceware.org/libffi/) 3.4.6), [OpenSSL](https://www.openssl.org) 3.0.13
(Apache-2.0) and [curl](https://curl.se) 8.5.0 (curl license), also built unmodified. The root
certificates are Mozilla's (MPL-2.0), from its `certdata.txt`. All of them are
linked against the [musl](https://musl.libc.org) C library (MIT license), which the ISO
also includes. Every release on the Releases page carries the matching GPL sources
(`busybox-1_36_1-source.tar.gz`, `bash-5.2.37.tar.xz`, `coreutils-9.4.tar.xz`).

The web browser is [NetSurf](https://www.netsurf-browser.org) 3.11 (GPL-2.0, with its own
libraries under the MIT license), modified for Vexa by `third_party/netsurf-vexa.patch`;
every release carries `netsurf-3.11-source.tar.gz` (its source bundle, the patch and
the build scripts). It's built with zlib, [libpng](http://www.libpng.org) 1.6.43,
[libjpeg-turbo](https://libjpeg-turbo.org) 2.1.5 (IJG and BSD licenses),
[FreeType](https://freetype.org) 2.13.2 (FreeType License), expat 2.6.1 (MIT) and
curl 8.5.0 on [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) 3.6
(Apache-2.0).

The X Window System in the ISO (the X.Org server 21.1 with Xvexa, libX11, libxcb and
the other X libraries, pixman, xkbcomp and xkeyboard-config, Xft and fontconfig with
expat, xterm 330 and ncurses 6.6) is under the MIT license and similar permissive
licenses. So are cairo (MPL-1.1 or LGPL-2.1, used under the MPL), HarfBuzz, fribidi
(LGPL-2.1), pixman, libpng, libepoxy, libffi and PCRE2 (BSD), and
[Mesa](https://mesa3d.org) 24.0.5 (MIT; also built for native programs, as
`/lib/libOSMesa.so`), [alsa-lib](https://www.alsa-project.org) 1.2.11
(LGPL-2.1) and alsa-utils 1.2.9 (`aplay` and `speaker-test`, GPL-2.0; their sources are in
every release), with [LLVM](https://llvm.org) 18.1.8 and its
libc++, libc++abi and libunwind (Apache-2.0 with LLVM exceptions); GTK 3, GLib, Pango,
gdk-pixbuf, ATK and at-spi2-core are under the LGPL-2.1 (or later), and D-Bus under the
AFL-2.1 or GPL-2.0; they are linked dynamically and unmodified, and every release
carries their sources. The exact upstream tarballs of all of these are listed in
`third_party/x11-sources.txt`, and the few changes made while building them (adding Xvexa to the server, `openpty` for
xterm on musl) are in `tools/build-x11.sh`. Xvexa itself is `third_party/xvexa/xvexa.c`.
Portions of this software are copyright © The FreeType Project (www.freetype.org), used
under the FreeType License. The DejaVu fonts are under the Bitstream Vera license (their
`LICENSE` file is installed next to them, in `/linux/usr/share/fonts/dejavu`).
