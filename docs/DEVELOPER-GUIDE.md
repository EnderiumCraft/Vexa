# Vexa developer guide

How to build Vexa, test it, find your way around the source, and write programs and
apps for it. For using Vexa, see the [user guide](USER-GUIDE.md); for how the kernel
works inside, see [ARCHITECTURE.md](ARCHITECTURE.md); for libvexa's functions, see the
[API reference](API.md).

- [Building](#building)
- [Running](#running)
- [Testing](#testing)
- [The source tree](#the-source-tree)
- [Writing a Vexa program](#writing-a-vexa-program)
- [Writing a desktop app](#writing-a-desktop-app)
- [App bundles](#app-bundles)
- [The desktop protocol](#the-desktop-protocol)
- [Linux programs in the ISO](#linux-programs-in-the-iso)
- [The kernel](#the-kernel)
- [Continuous integration and releases](#continuous-integration-and-releases)
- [Conventions](#conventions)

## Building

You need a Linux machine (or WSL) with:

| Tool | Debian/Ubuntu package |
| --- | --- |
| GCC or Clang for x86_64, GNU ld | `build-essential`, `binutils` |
| xorriso (makes the ISO) | `xorriso` |
| QEMU | `qemu-system-x86` |
| git, make, curl | `git`, `make`, `curl` |
| Python 3 and UEFI firmware (for the tests) | `python3`, `ovmf` |
| mke2fs, e2fsck (test disks) | `e2fsprogs` |
| musl's compiler and Linux headers (Linux programs) | `musl-tools`, `linux-libc-dev` |
| Meson, Ninja, pkg-config, bison, gperf (X and GTK) | `meson`, `ninja-build`, `pkg-config`, `bison`, `gperf` |
| GLib's code generators (GTK) | `libglib2.0-dev-bin`, `gtk-update-icon-cache` |

X.Org's packages want a newer Meson than some distributions have; CI uses
`pipx install meson==1.12.1` and `pipx inject meson packaging`.

```sh
make              # build/vexa.iso: the kernel, libvexa, the programs and the Linux files
make programs     # just Vexa's own programs (fast)
make LINUX_COMPAT=0   # a kernel without the Linux subsystem (and no /linux)
make clean        # removes build/ (everything built, the Linux programs too)
make distclean    # also removes the downloaded sources and Limine
```

The first `make` downloads the Limine bootloader and the sources of the Linux programs
(BusyBox, bash, coreutils, Python, the X and GTK stack) and builds them with musl. That
takes a long time (the X and GTK stack most of all); later builds reuse them. Each
third-party build is stamped, and rebuilt only when its sources list or build script
changes.

What goes into the ISO:

- `build/vexa-kernel`, the kernel, loaded by Limine (`limine.conf` has the boot menu)
- `build/initramfs.tar`, the starting root file system: `rootfs/`, Vexa's programs in
  `/bin`, `libvexa.so` and `vexa-ld.so` in `/lib`, the apps in `/apps`, and the small
  part of `/linux` that changes (`etc`, `var`, `root`)
- `/linux` on the CD: the Linux programs and libraries (`bin`, `lib`, `sbin`, `usr`),
  read from the CD when they're used (the initramfs has links to `/cdrom/linux/...`)

## Running

```sh
make run            # QEMU with a window; the serial log (the kernel's messages) in your terminal
make run-disk       # the same, with build/my-disk.img (ext2) at /mnt/vda1, kept between runs
make run-nographic  # no window: the serial console only (Ctrl-A then X quits)
```

`QEMU=...` and `QEMU_NET=...` change the QEMU binary and its network options. The ISO
also boots on real PCs from a USB stick or CD, with BIOS or UEFI.

## Testing

`make test` is the main test. It boots the ISO in QEMU four ways, **at once**:

| Target | Machine |
| --- | --- |
| `test-bios` | BIOS, 1 CPU, 512 MiB, with virtio, SATA and NVMe test disks |
| `test-uefi` | UEFI (OVMF), 4 CPUs, 6 GiB, `-cpu max` (AVX, SMEP, SMAP) |
| `test-safe` | the safe mode boot (no ACPI, the legacy PIC and PIT) |
| `test-native-boot` | a kernel built with `LINUX_COMPAT=0` |

Each boot is driven by `tools/qemu-smoke-test.py`: it types commands on QEMU's virtual
keyboard, moves and clicks the virtual mouse, and waits for the expected text in the
serial log (the console is mirrored there, and the desktop and Files print what they
do). After the boot it checks the test disks with `e2fsck`. It uses KVM when
`/dev/kvm` can be opened, and emulation otherwise (`--accel` picks). `TEST_JOBS=1 make
test` runs the boots one after another.

While working on one part, boot once and run only its checks:

```sh
make test-quick ONLY=desktop        # about 2 minutes without KVM
make test-quick ONLY=shell,network
```

The sections are `shell`, `network`, `desktop`, `linux`, `x`, `linux-net` and `disks`.
In this mode each check looks only at what came after the step before it, since earlier
checks were skipped.

**The test list.** `TYPED_COMMANDS`, `LINUX_COMMANDS` and `DISK_COMMANDS` in
`tools/qemu-smoke-test.py` are lists of `(command, expected text, seconds[, count])`:

```python
("ls /apps", "Terminal.vxapp", 10),                  # typed at vsh's prompt
("@sendkey ctrl-alt-f", 'desktop: window 5', 30),    # a QEMU monitor command
("@type /apps", '"/apps - Files"', 10),              # typed into a window
("@mouse_move 100 50", None, 5),                     # moves are relative
("@double-click", "desktop: starting Files", 10),
("#desktop",),                                       # a section starts
```

Commands without `@` wait for the shell's prompt first. `count` means the text must be
in the whole log that many times (typed commands are echoed, which is why some are 2).
QEMU drops part of long mouse moves, so they're split into steps of about 300 pixels;
the pointer starts in the middle of the 1280x800 screen. New windows open at known
places (the desktop cascades them by window number), which the tests rely on.

`--screenshot file.png` saves the screen at the end, and `--keep-log` prints the serial
log. A small script that imports the module, sets `TYPED_COMMANDS` to a few steps and
calls `main()` is a quick way to look at one thing.

## The source tree

```
Makefile              the build, the ISO, QEMU and the tests
limine.conf           the boot menu
kernel/               the kernel (see ARCHITECTURE.md)
  src/arch/x86_64/    CPU setup, interrupts, APIC, timer, FPU state, system calls, SMP
  src/core/           memory, scheduler, processes, signals, pipes, VFS, ELF, ACPI, monitor
  src/dev/            console, terminals, keyboard, mouse, display, PCI, virtio, AHCI, NVMe
  src/fs/             ext2, ISO 9660, tmpfs, devfs, procfs, the initramfs
  src/net/            Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, sockets
  src/personality/    the native system calls (vexa/) and the Linux subsystem (linux/)
abi/vexa/abi.h        system call numbers, structures and errors: kernel and libvexa share it
libvexa/              Vexa's C library (libvexa.so) and dynamic loader (ld/)
userland/<name>/      Vexa's programs, one directory each, built into /bin
apps/<Name>.vxapp/    the desktop apps' bundles (Info.conf, icons)
rootfs/               files for the root file system (/etc, /share: pictures, fonts)
third_party/          Xvexa (Vexa's X server), stb_truetype, BusyBox's configuration, the X sources list
tools/                build scripts, the test harness, disk images, icons
tests/                Linux test programs and test disk contents
docs/                 this documentation
```

## Writing a Vexa program

Vexa programs are C, built against **libvexa**, Vexa's own C library. It has the usual
C basics (`<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`: `printf`, `malloc`,
`qsort`, `strtol`, `getenv`...) and Vexa's own interfaces under `<vexa/...>` (see the
[API reference](API.md)). It isn't POSIX: files are handles from `vx_open`, programs
start with `vx_spawn`, and errors are negative `VX_E*` numbers.

To add a program, make a directory in `userland/` with its C files:

```c
/* userland/greet/main.c */
#include <stdio.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    struct vx_system_info info;
    vx_system_info(&info);
    printf("Hello, %s! This is Vexa %s.\n", argc > 1 ? argv[1] : "world", info.version);
    return 0;
}
```

`make` builds every directory in `userland/` into `/bin/<name>`, so `greet Ada` works
at the shell after the next build. Programs are position-independent executables that
use `/lib/libvexa.so` through `/lib/vexa-ld.so`; the kernel knows them from Linux
programs by a note in the ELF file (see ARCHITECTURE.md). Warnings are errors
(`-Wall -Wextra -Werror`).

A few things to know:

- **Errors**: every `vx_*` call returns a negative `VX_E*` on failure;
  `vx_strerror(error)` describes it. `vx_open` and friends return a handle (0, 1 and 2
  are the standard input, output and error).
- **Starting programs**: `vx_spawn(path, &spawn)` with argv, the environment and which
  handles the child gets; `vx_wait` for its exit code. `environ` is the environment.
- **Threads**: `<vexa/thread.h>` (`vx_thread_create`, `vx_thread_join`, `vx_mutex`).
- **Signals**: Vexa programs can't catch signals; they can ignore them
  (`vx_signal(VX_SIGINT, VX_SIGNAL_IGNORE)`) or be stopped by them.
- **Output from desktop apps**: a program the desktop started writes to the desktop's
  output, which is the serial log; `printf("myapp: ...")` lines are how the tests see
  what an app did.

## Writing a desktop app

`<vexa/gui.h>` opens windows on the desktop, draws into them and reads events:

```c
#include <stdio.h>
#include <vexa/gui.h>

int main(void) {
    struct vx_window *w = vx_window_create_flags("Counter", 300, 120, VX_WINDOW_RESIZABLE);
    if (!w) {
        return 1; /* No desktop running. */
    }
    int clicks = 0;
    for (;;) {
        struct vx_surface *s = &w->surface;
        char text[32];
        snprintf(text, sizeof(text), "Clicks: %d", clicks);
        vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_WINDOW);
        vx_draw_text(s, 20, 20, text, VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_button(s, 20, 60, 100, 28, "Click me", false);
        vx_window_present(w, 0, 0, s->width, s->height);

        struct vx_gui_event e;
        if (vx_gui_wait(&e, -1) <= 0 || e.type == VX_GUI_CLOSE) {
            return 0;
        }
        if (e.type == VX_GUI_POINTER && (e.buttons & 1) && vx_inside(e.x, e.y, 20, 60, 100, 28)) {
            clicks++; /* (A real app would count presses, not every event with the button down.) */
        }
        if (e.type == VX_GUI_RESIZE) {
            vx_window_resize(w, e.width, e.height);
        }
    }
}
```

- A window's pixels (`w->surface`, 0xRRGGBB) are memory shared with the desktop:
  draw, then `vx_window_present` the part that changed.
- Events: `VX_GUI_KEY` (Linux key codes `VX_KEY_*`, `value` 1 down, 0 up, 2 repeat, and
  the `character` typed), `VX_GUI_POINTER` (position in the window, `buttons`: bit 0
  left, 1 right, 2 middle, `wheel`), `VX_GUI_CLOSE`, `VX_GUI_FOCUS`, `VX_GUI_RESIZE`.
  Modifier keys (Ctrl, Shift, Alt) come as key events too: keep track of them.
- `vx_gui_wait(&e, timeout)` also takes a timeout (0: don't wait), for animations or
  work done between events; `vx_gui_handle()` gives the connection's handle for
  `vx_poll` with other handles.
- Text is UTF-8 and drawn smooth (TrueType). `vx_draw_text` is the apps' font in
  16-pixel lines; measure with `vx_text_width` (characters aren't all as wide), and
  for other sizes or bold, `vx_text(s, vx_font(VX_FACE_BOLD, 24), ...)`. Grids of
  characters (terminals, editors) use `vx_draw_char`: monospaced cells, 8 by 16.
  `character` in key events is Unicode: encode it with `vx_utf8_encode`.
- Widgets in Vexa's look: `vx_draw_button`, `vx_draw_field` and `vx_field_key` (a text
  field), `vx_draw_menu` (a right-click menu), `vx_draw_text_fit`, `vx_draw_outline`, and
  the `VX_COLOR_*` theme colors.
- `vx_window_set_cursor(w, VX_CURSOR_TEXT)` changes the pointer over the window (an
  I-beam over text, say).
- Drag and drop: while a button pressed in the window is down, pointer events keep
  coming even outside it. Let go outside, and `vx_window_drag_files(w, paths, n, copy)`
  hands the files to the desktop, which puts them where the pointer is (the desktop, or
  another window: it gets a `VX_GUI_DROP`, whose paths `vx_drop_paths` reads).
- Pictures: `vx_image_load` (PNG, BMP, PPM), `vx_blit_scaled`, and with `VX_IMAGE_ALPHA`,
  `vx_blit_alpha` for icons with transparency.
- `vx_notify("App: something happened")` shows a desktop notification.
- Use the `VX_COLOR_*` colors (or `vx_theme`) rather than your own, and draw again on
  `VX_GUI_THEME`: then the app follows Settings' dark and light themes and accent.
- Settings of your own go in a file in `/etc` through `<vexa/settings.h>`: they're
  kept on disk like the system's.

The desktop's own apps in `userland/` (`about` is the smallest, `files` the largest)
are complete examples.

## App bundles

To appear in the desktop's menu and Files as an app, a program needs a **bundle** in
`apps/`:

```
apps/Counter.vxapp/
    Contents/
        Info.conf
        Resources/icon.png     48x48, with transparency
```

```ini
# apps/Counter.vxapp/Contents/Info.conf
name=Counter            # the name people see
executable=counter      # userland/counter, built and moved into Contents/Vexa
icon=icon.png           # in Contents/Resources
opens=txt md            # file extensions it opens (* for anything); optional
shortcut=Ctrl+Alt+C     # a desktop shortcut (Ctrl+Alt+ a letter); optional
menu=6                  # its place in the Vexa menu (leave it out: not in the menu)
desktop=yes             # an icon on the desktop; optional
kind=linux              # only for Linux programs: listed with them in the menu
```

The build copies `apps/` to `/apps`, moves each app's program from `/bin` into its
bundle (`Contents/Vexa/counter`) and leaves a link in `/bin`, so `counter` still works
at the shell. `executable` may also be an absolute path (as `XTerm.vxapp`'s is,
`/linux/usr/bin/xsession`); an app whose program isn't there is left out.

`<vexa/app.h>` reads bundles: `vx_app_list` (what the desktop's menu shows),
`vx_app_find` (`open -a`), `vx_app_for_file` (which app opens a file: the first that
lists its extension, else the first with `opens=*`), and `vx_app_open` (starts one, with
a file).

The icons are drawn by `tools/make-app-icons.py` (it needs Pillow), which writes each
bundle's `icon.png` and Files' own pictures (`Files.vxapp/Contents/Resources`: kinds of
files and places). The PNGs are kept in the repository, so building doesn't need
Pillow.

Apps can also be installed while Vexa runs: copying a bundle into `/apps` is enough;
the desktop looks there every two seconds.

## The desktop protocol

The desktop (`userland/desktop`) listens on the local socket `/run/desktop`. Programs
and the desktop send each other fixed-size messages (`struct desktop_message` in
`<vexa/desktop.h>`): a type, a window id, four numbers and some text. `<vexa/gui.h>`
wraps all of it; only the desktop and Xvexa use it directly.

- A window's pixels are a file the program makes in `/run/shm` and maps; the
  desktop maps the same file, so presenting copies nothing.
- **Program to desktop**: `CREATE` (size, flags: `RESIZABLE`, `POPUP` for menus and
  tooltips, `UNDECORATED` for windows that draw their own title bar), `PRESENT` (a
  rectangle), `TITLE`, `DESTROY`, `BUFFER` (a new buffer after a resize), `MOVE`, `INFO`
  (the screen's size), `NOTIFY`, `RELOAD` (read `/etc/desktop.conf` again), `WM`
  (what a program asks of a window manager: maximize, restore, minimize, activate, or
  start dragging to move or resize), `CURSOR` (the pointer's shape over the window),
  `DRAG` (files dragged out and let go: a list file in `/tmp`) and `LOCK`.
- **Desktop to program**: `CREATED`, `KEY`, `POINTER`, `CLOSE`, `FOCUS`, `CONFIGURE`
  (please be this size), `RESIZED`, `MOVED`, `INFO_REPLY`, `STATE` (maximized,
  minimized), `THEME` (read the theme again) and `DROP` (files dropped on the window).

The desktop also reads `/etc/desktop.conf` (wallpaper, clock, time zone, lock screen...)
and builds its menu, icons and shortcuts from `/apps`, `/home/Desktop` and
`/linux/usr/share/applications`.

The desktop is in parts, in `userland/desktop/`:

| File | What it does |
| --- | --- |
| `main.c` | windows (frames, stacking, focus, moving, resizing, snapping, animations), the panel and the Vexa menu, the protocol, the keyboard (layouts, dead keys, shortcuts) and the pointer, the display |
| `shell.h` | what the parts share |
| `look.c` | shadows, round corners, smooth scaling, blur, the pointer's shapes |
| `icons.c` | the icons on the desktop: apps, the Desktop folder's files, the Trash |
| `switcher.c` | Alt+Tab |
| `search.c` | search (Ctrl+Space) |
| `clock.c` | the calendar and the notifications under the clock |
| `shot.c` | screenshots |
| `lock.c` | the screensaver and the lock screen |

It prints what it does to its standard output (the serial log, when it's started from
the console): `desktop: window 3 "Settings" (860x580) at 144,138`, `desktop: snapped
window 1 to the top left`, `desktop: locked`... The tests wait for these lines.

## Linux programs in the ISO

The Linux subsystem (`kernel/src/personality/linux`) runs x86_64 Linux programs built
with musl. What's in the ISO is built from source with musl by the Makefile and
`tools/`:

- BusyBox (`third_party/busybox.config`), GNU bash, GNU coreutils and Python 3.12
  (targets `busybox`, `bash`, `coreutils`, `python`)
- the X and GTK stack (`tools/build-x11.sh`, sources in `third_party/x11-sources.txt`),
  with `tools/musl-cc-wrapper.sh` and `tools/musl-cxx-wrapper.sh` for the compilers
- `tools/make-linux-root.sh` puts it all together into `build/linux-root`, which
  becomes `/linux`; files of Vexa's own for it are in `tools/linux-files`
- Linux test programs in `tests/linux` are built with `musl-gcc` too

To add a Linux program: build it with musl (static, or dynamic against the libraries
already in `/linux`), then copy it into the Linux root in `tools/make-linux-root.sh`.
A `.desktop` file in `/linux/usr/share/applications` puts an X program in the menu. The
sources of GPL and LGPL programs must be attached to releases (CI does this for what's
there now).

## The kernel

The kernel is freestanding C (no floating point, no red zone, higher half), built
with `-Wall -Wextra -Werror`. [ARCHITECTURE.md](ARCHITECTURE.md) describes its parts;
some starting points:

- the boot sequence is `kernel/src/kmain.c`
- system calls: numbers and structures in `abi/vexa/abi.h`, the native ones in
  `kernel/src/personality/vexa/syscalls.c`, the Linux ones in
  `kernel/src/personality/linux/`
- devices are files under `/dev` (`kernel/src/fs/devfs.c`); a device request is
  `vx_control`
- the kernel monitor (`kernel/src/core/monitor.c`) is also reachable from programs
  through `vx_kernel_command` (that's what `sys` does)

To add a system call: give it a number in `abi/vexa/abi.h`, handle it in
`personality/vexa/syscalls.c`, and add a wrapper to `libvexa/src/syscall.c` and
`<vexa/syscall.h>`.

## Continuous integration and releases

`.github/workflows/build.yml` runs on every push: it installs the tools, restores the
cached X and GTK build (keyed on its sources and scripts), builds the ISO, runs
`make test` (with KVM), and publishes the result:

- **Latest build**, replaced on every push
- a **versioned release** (`vX.Y.Z`) whenever `VEXA_VERSION` in
  `kernel/include/vexa/version.h` changes

Each release carries the ISO and the sources of the GPL and LGPL programs in it. To
make a release, change `VEXA_VERSION` in the same commit as the changes it describes,
and add them to `docs/ROADMAP.md`.

## Conventions

- C11 (GNU), 4 spaces, braces on the same line, lines up to about 100 characters.
- Comments are plain sentences that say what and why; each program starts with a
  comment on what it is and how it's used.
- Names: `vx_` for libvexa and the ABI, `VX_` for constants; the kernel's own
  functions have their subsystem's prefix (`vfs_`, `pty_`, `tcp_`...).
- Every new feature comes with a check in `make test` when it can be driven from the
  keyboard or mouse; programs print what they did (`files: pasted ...`) so the tests
  can see it.
- Docs change with the code: README.md for what Vexa can do, docs/ROADMAP.md for what's
  done, this guide and the API reference for how.
