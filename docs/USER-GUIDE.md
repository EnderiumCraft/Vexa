# Vexa user guide

This guide is for using Vexa: starting it, the shell and its commands, the desktop
and its apps, and the Linux programs that come with it. For building Vexa and
writing programs for it, see the [developer guide](DEVELOPER-GUIDE.md).

- [Starting Vexa](#starting-vexa)
- [The shell](#the-shell)
- [Commands](#commands)
- [Files and folders](#files-and-folders)
- [The desktop](#the-desktop)
- [Apps](#apps)
- [Files, the file manager](#files-the-file-manager)
- [Settings](#settings)
- [Linux programs](#linux-programs)
- [X and GTK programs](#x-and-gtk-programs)
- [Network](#network)
- [Disks and CDs](#disks-and-cds)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [When something goes wrong](#when-something-goes-wrong)

## Starting Vexa

Vexa boots from a CD image (`vexa.iso`) on x86_64 PCs and virtual machines, with
BIOS or UEFI firmware. Download the newest ISO from the
[Releases page](https://github.com/EnderiumCraft/Vexa/releases) (or build it; see the
developer guide) and start it in QEMU:

```sh
qemu-system-x86_64 -M q35 -m 512M -cdrom vexa.iso
```

More memory (`-m 2G`) helps the Linux programs, and `-enable-kvm` (on a Linux host with
KVM) makes everything much faster. For a network card, add
`-netdev user,id=n0 -device virtio-net-pci,netdev=n0`; for more CPUs, `-smp 4`.

The boot menu (Limine) has these entries:

| Entry | What it's for |
| --- | --- |
| **Vexa** | the normal boot |
| **Vexa (safe mode)** | machines where the normal boot fails: ignores ACPI and uses only the oldest interrupt and timer hardware |
| **Vexa (kernel monitor)** | the kernel's own command line, instead of the normal programs, for when something is badly broken |

The kernel options behind safe mode (`acpi=off`, `noapic`), and `nosmp` (use only the
first CPU core), can also be written into `limine.conf`.

While it starts, the kernel prints what it finds (memory, CPUs, disks, the network).
Then `vinit`, the first program, shows a welcome message and starts the shell:

```
vexa:/>
```

## The shell

`vsh` is Vexa's shell. Type a command and press Enter.

| You type | What happens |
| --- | --- |
| `ls /bin` | runs a program (found in `$PATH`, or by its path) with arguments |
| `ls /bin \| cat` | a pipe: one program's output is the next one's input |
| `cat < notes.txt` | reads the input from a file |
| `echo hi > notes.txt` | writes the output to a file (`>>` adds to the end, `2>` is for errors) |
| `fpu-stress &` | runs a program in the background |
| `cd /tmp ; ls` | one command after the other |
| `echo 'a b' "c $HOME" \$` | quoting: `'...'` as is, `"..."` with `$NAME` replaced, `\` for one character |
| `echo $?` | the last command's exit code |

Built into the shell: `cd`, `exit`, `export NAME=value`, `unset NAME`, `env` and `help`.

Editing a line: Backspace, and Ctrl-U erases the whole line. Ctrl-C stops the
program in front (not the shell), Ctrl-\\ stops it harder, and Ctrl-D means "end of
input" to a program reading the keyboard.

## Commands

Vexa's own programs are in `/bin` (`ls /bin` lists them).

### Files

| Command | What it does |
| --- | --- |
| `ls [-l] [-a] [path...]` | lists folders; `-l` shows sizes, kinds and where links point, `-a` names starting with a dot |
| `cat [file...]` | shows files (or the input) |
| `cp [-r] from to`, `cp [-r] file... folder` | copies files; `-r` copies folders with everything in them |
| `mv from to`, `mv path... folder` | renames or moves |
| `rm [-r] path...` | removes files and empty folders; `-r` removes folders with everything in them |
| `mkdir [-p] path...` | makes folders; `-p` makes missing parents too |
| `ln -s target name` | makes a symbolic link |
| `pwd` | the current folder |
| `open file`, `open -a App [file]` | opens something as a double click would (see [Apps](#apps)) |

### Processes and the system

| Command | What it does |
| --- | --- |
| `ps` | the running processes |
| `kill [-signal] id...` | sends a signal (`kill -9 12`); SIGTERM unless told |
| `sleep seconds` | waits (fractions allowed: `sleep 0.5`) |
| `uptime` | time since boot, CPUs and memory |
| `hello` | says hi, with Vexa's version |
| `clear` | clears the screen |
| `sys [command]` | the kernel monitor's commands: `sys disks`, `sys mount`, `sys pci`, `sys cpu`, `sys mem`, `sys memmap`, `sys memtest`, `sys threads`; `sys` alone lists them |

### Network

| Command | What it does |
| --- | --- |
| `net` | network interfaces, their addresses and traffic |
| `fetch [-o file] http://host/path` | downloads a web page (HTTP) and shows it, or saves it with `-o` |

### Graphics and input

| Command | What it does |
| --- | --- |
| `desktop` | starts the desktop (see [The desktop](#the-desktop)) |
| `input`, `input watch N [count]` | lists keyboards and mice; shows what one reports |
| `notify text` | shows a notification on the desktop |
| `term`, `files`, `edit [file]`, `view image`, `settings`, `about` | the desktop's apps, from a terminal on the desktop |

### Checks

These check that parts of Vexa work, and print what they find:

| Command | What it checks |
| --- | --- |
| `hello-world` | the very first Vexa program: user mode and system calls |
| `crash` | misbehaves on purpose: Vexa must stop it and carry on |
| `fs-test` | the file system calls (in `/tmp`, and on a disk at `/mnt/vda1` if there is one) |
| `fpu-stress` | that each program's vector registers survive; run several with `&` |
| `thread-test [exit]` | threads, a mutex and waits |
| `socket-test` | sockets over the loopback network |

## Files and folders

Vexa's file system starts in memory and has these folders:

| Folder | What's in it |
| --- | --- |
| `/bin` | Vexa's programs (the apps' programs are links into their bundles) |
| `/lib` | `libvexa.so` and Vexa's dynamic loader, `vexa-ld.so` |
| `/apps` | the desktop's apps, as `.vxapp` bundles |
| `/etc` | settings: `motd`, `desktop.conf`, `hosts`, `resolv.conf` |
| `/share/pictures` | pictures (the wallpaper, `meadow.png`, and `aurora.png`) |
| `/tmp` | scratch space |
| `/Trash` | what Files moved to the Trash |
| `/dev` | devices: the console, terminals (`/dev/pts`), `/dev/input`, `/dev/display0` |
| `/proc` | the processes and the system, in Linux's format (`cat /proc/meminfo`) |
| `/run` | the desktop's socket and shared window buffers |
| `/mnt` | disks and CDs (`/mnt/vda1`, `/mnt/cd0`, ...) |
| `/cdrom` | the boot CD |
| `/linux` | the Linux programs' own file tree (`/linux/usr/bin`, ...) |

Everything outside `/mnt` is in memory: it starts fresh at every boot. To keep files,
put them on a disk (see [Disks and CDs](#disks-and-cds)).

## The desktop

Type `desktop` at the shell. The desktop takes the screen and opens a terminal window.
Ctrl+Alt+Q goes back to the text console.

![The desktop](desktop-screenshot.png)

**The panel** along the top has:

- the **Vexa menu** (top left): the apps, then the Linux programs, then "Back to the
  console"; each shows its keyboard shortcut
- a **button for each window**: a click shows it (or minimizes it, if it's in front)
- the **clock** (Settings changes its format and time zone)

**Desktop icons** on the left start apps with a double click: Terminal, Files, Editor,
Settings and XTerm. A **right click** on the desktop opens a menu (New Terminal, Open
Files, Change Wallpaper, About Vexa); on an icon, Open and Show in Files.

**Windows**

- drag a window by its **title bar**; a click raises it and gives it the keyboard
- the buttons on the right of the title bar **minimize**, **maximize** and **close** it;
  a double click on the title bar maximizes it
- drag the **right or bottom edge** (or the corner) to resize it
- drag a window to the **top edge** of the screen to maximize it, or to the **left or
  right edge** to make it fill that half; dragging a maximized window brings back its
  old size
- **Alt+Tab** goes to the next window

**Notifications** appear at the top right for a few seconds (`notify Hello!` shows one).

**Keyboard shortcuts** on the desktop: Ctrl+Alt+T a terminal, Ctrl+Alt+F Files,
Ctrl+Alt+E the text editor, Ctrl+Alt+X an xterm, Alt+Tab the next window, Ctrl+Alt+Q
back to the console.

## Apps

The desktop's apps are **bundles**, as on macOS: an app is a folder whose name ends
in `.vxapp`, kept in `/apps`. Files shows each one as a single app with its icon.

| App | Bundle | What it is |
| --- | --- | --- |
| Terminal | `Terminal.vxapp` | a terminal window running the shell |
| Files | `Files.vxapp` | the file manager |
| Text Editor | `Editor.vxapp` | a text editor; opens any file |
| Image Viewer | `Viewer.vxapp` | shows PNG, BMP and PPM pictures |
| Settings | `Settings.vxapp` | wallpaper, clock and time zone |
| About Vexa | `About.vxapp` | the version, and how the system is doing |
| XTerm | `XTerm.vxapp` | an xterm (a Linux X program; only when the Linux files are there) |

**Opening things.** `open` does what a double click in Files does:

```sh
open /share/pictures/aurora.png   # with the app for .png files: Image Viewer
open /tmp                         # a folder: in Files
open /apps/Settings.vxapp         # starts the app
open -a Editor notes.txt          # with a given app (its name or bundle name)
open -a Terminal                  # just starts it
```

**Installing an app** is copying its bundle into `/apps` (with Files, or
`cp -r MyApp.vxapp /apps/`). Within a couple of seconds the desktop adds it to the
menu (and, if it asks for one, a desktop icon). Removing the bundle uninstalls it.

What's inside a bundle, and how to make one, is in the developer guide.

### Terminal

A terminal window running `vsh` on a pseudo-terminal. It shows colors and moves the
cursor the way the console does, so full-screen programs like `vi`, `less` and `top`
(from BusyBox) work. It can be resized; the program inside is told its new size.

### Text Editor

`edit [file]` opens a file, or a new one. Arrows, Home, End, Page Up and Page Down
move; a click puts the cursor there, and the mouse wheel scrolls. Ctrl+S saves (asking
for a name the first time), Ctrl+Q or the close button quits.

### Image Viewer

`view file` shows a PNG (8 bits per channel, not interlaced), BMP (24 or 32 bits) or
PPM (P6) picture, fitted to the window. `+` and `-` zoom, `0` fits it again.

### About Vexa

The version, the number of CPUs, memory in use, time since boot, the number of
processes and the network address, updated every second.

## Files, the file manager

Files works much like Finder on macOS. Start it from the menu, its desktop icon,
Ctrl+Alt+F, or `files [folder]`.

![Files](files-screenshot.png)

**The window**

- the **sidebar**: Places (Vexa, the whole system; Apps; Pictures; Temporary; Trash)
  and Disks (the boot CD, and disks in `/mnt`)
- the **toolbar**: Back and Forward (`<`, `>`), Up (`^`), the location (click it, or
  Ctrl+L, to type one), **List** and **Icons**, and **Search**
- the **folder**, as a list (Name, Kind, Size, Modified: click a heading to sort by it,
  again to reverse) or as icons, where pictures show as thumbnails
- the **status bar**: how many items, how many are selected, or what just happened

**Selecting.** A click selects one thing; Ctrl-click adds or takes away; Shift-click
selects everything in between; Ctrl+A selects all; Esc, or a click on nothing, selects
nothing. The arrow keys move (with Shift, they extend the selection). Typing the first
letters of a name goes to it.

**Opening.** A double click or Enter opens: a folder goes into it, an app starts, a
program in `/bin` runs, and a file opens with the app for its type (pictures in Image
Viewer, anything else in the Text Editor). Backspace (or Alt+Up) goes up; Alt+Left and
Alt+Right go back and forward.

**Quick Look.** Space shows the selected picture or text file in a panel over the
window, without opening an app; Space or Esc closes it.

**Search.** Ctrl+F (or a click in the search field) and type: the folder shows only what
has that in its name. Enter keeps the search; Esc clears it.

**Changing things.** A right click opens a menu with everything you can do; so does the
keyboard:

| Action | Keys | What it does |
| --- | --- | --- |
| Copy, Cut | Ctrl+C, Ctrl+X | puts the selection on the clipboard (shared by all Files windows) |
| Paste | Ctrl+V | copies (or moves, after Cut) it here; a name that's taken gets a number ("notes 2.txt") |
| Duplicate | Ctrl+D | a copy next to the original |
| Make Alias | (menu) | a symbolic link to it ("name alias") |
| Rename | F2 | type the new name, Enter (Esc keeps the old one) |
| New Folder | Ctrl+Shift+N | "untitled folder", named right away |
| New Text Document | (menu) | an empty "untitled.txt", named right away |
| Move to Trash | Delete | moves it to `/Trash` |
| Delete Immediately | Delete, in the Trash | removes it for good (asks first) |
| Empty Trash | (menu, in the Trash) | removes everything in the Trash (asks first) |
| Get Info | Ctrl+I | kind, size (a folder's files counted), where, when modified, where an alias points, an app's program and file types, the app a file opens with |
| Show Package Contents | (menu, on an app) | opens the bundle as a folder |
| Open in Text Editor | (menu, on a file) | opens any file as text |
| Open in Terminal | (menu, on nothing) | a terminal in this folder |
| Show Hidden Files | Ctrl+H | shows (or hides) names starting with a dot |
| List, Icons | Ctrl+1, Ctrl+2 | the two views |

**Dragging.** Drag the selection onto a folder in the list, or onto a place in the
sidebar, to move it there; hold Ctrl to copy instead. Dropping onto Trash moves it to the
Trash.

## Settings

Settings (in the menu, or a right click on the desktop, "Change Wallpaper...") has:

- **Wallpaper**: a picture (the default is `/share/pictures/meadow.png`; type the path
  of any PNG, BMP or PPM file) or one of five gradients (Dusk, Ocean, Forest, Sunset,
  Graphite)
- **Clock**: 24-hour or 12-hour
- **Time zone**: an offset from UTC, in half hours

Apply saves them in `/etc/desktop.conf` and the desktop changes at once. The file is
plain text:

```
wallpaper=image
wallpaper_image=/share/pictures/meadow.png
clock=24
utc_offset=120
```

## Linux programs

Vexa runs Linux programs (built for x86_64 Linux, with the musl C library) through
its Linux subsystem. They keep their own file tree under `/linux`, and look there
first: `/linux/usr/bin/bash` is what a Linux program sees as `/usr/bin/bash`. From
`vsh`, their names work like Vexa's:

| Program | What it is |
| --- | --- |
| `bash` | GNU bash 5.2 (`exit` goes back to `vsh`) |
| `busybox`, `sh` | BusyBox 1.36: `vi`, `less`, `grep`, `sed`, `awk`, `find`, `top`, `ps`, `wget`, `ping`, `ifconfig`, `tar`, and about 300 more (`busybox` lists them) |
| GNU coreutils 9.4 | `ls`, `cp`, `sort`, `df`, `timeout`, ... (inside bash, `ls` is coreutils') |
| `python3` | Python 3.12, with threads, `subprocess`, `multiprocessing`, sockets, `urllib`, `asyncio` |
| `xterm`, `gtk3-demo`, ... | X and GTK programs (see below) |

They get the usual Linux interfaces: processes (`fork`, `exec`), signals, threads,
pipes, terminals, sockets (TCP, UDP, local sockets that pass open files), shared memory
and `/proc`. `pthread-test`, `bsd-socket-test`, `memfd-test` and `python-net-test.py`
check parts of it.

## X and GTK programs

X programs run on Vexa's desktop through **Xvexa**, Vexa's X server: each X window
becomes a desktop window of its own, which you move, resize, maximize and close like
any other. Ctrl+Alt+X (or the XTerm icon) opens an xterm, with DejaVu fonts.

From a terminal, `xrun program` starts any X program (and the X server, the first
time):

```sh
/linux/usr/bin/xrun xterm
/linux/usr/bin/xrun gtk3-demo
```

GTK 3 programs appear in the Vexa menu (from the `.desktop` files in
`/linux/usr/share/applications`): **GTK 3 Demo**, **GTK 3 Widget Factory** and **GTK 3
Icon Browser**. GTK windows draw their own title bars; their buttons (minimize,
maximize, close) and dragging work through Xvexa, which stands in for a window manager.

## Network

With a network card (QEMU's `virtio-net-pci`), Vexa gets an address by DHCP when it
starts. In QEMU's user-mode network: Vexa is 10.0.2.15, the router (and your machine)
is 10.0.2.2, and DNS goes through 10.0.2.3.

```sh
net                           # interfaces and addresses
fetch http://example.com/     # a web page (Vexa's program)
wget -O - http://example.com/ # the same, with BusyBox
ping -c 3 10.0.2.2            # BusyBox's ping
python3 -c "import urllib.request; print(urllib.request.urlopen('http://example.com').status)"
```

There is no HTTPS yet (no TLS library), and no IPv6.

## Disks and CDs

Vexa mounts every ext2 file system and CD it finds at `/mnt/<disk>`: `/mnt/vda1` (a
virtio disk's first partition), `/mnt/sda1` (SATA), `/mnt/nvme0n1` (NVMe), `/mnt/cd0`
(a CD). The boot CD is also `/cdrom`. `sys disks` lists the disks and partitions, and
`sys mount` what's mounted. Files shows them in its sidebar, under Disks.

It drives virtio-blk, AHCI (SATA disks and CD/DVD drives) and NVMe, reads GPT and MBR
partition tables, reads and writes **ext2**, and reads CDs (ISO 9660 with Rock Ridge).
ext4 disks are refused (their extra features aren't supported yet). ext2 has no journal:
turning the machine off in the middle of writing can leave a disk that needs `e2fsck`
(on Linux).

A disk to try, on Linux:

```sh
truncate -s 64M disk.img && mke2fs -t ext2 disk.img
qemu-system-x86_64 -M q35 -m 512M -cdrom vexa.iso -drive file=disk.img,if=virtio,format=raw
```

Then `ls /mnt/vda1`, and what you save there is still there next time.

## Keyboard shortcuts

**Console and terminals**: Ctrl-C stop the program, Ctrl-\\ stop it harder, Ctrl-D end of
input, Ctrl-U erase the line.

**Desktop**

| Keys | What they do |
| --- | --- |
| Ctrl+Alt+T | a new terminal |
| Ctrl+Alt+F | Files |
| Ctrl+Alt+E | the text editor |
| Ctrl+Alt+X | an xterm |
| Alt+Tab | the next window |
| Ctrl+Alt+Q | back to the text console |
| Esc | closes the Vexa menu |

**Files**: Enter open, Space Quick Look, Backspace or Alt+Up up, Alt+Left back,
Alt+Right forward, Ctrl+C copy, Ctrl+X cut, Ctrl+V paste, Ctrl+D duplicate, F2 rename,
Delete move to Trash, Ctrl+I Get Info, Ctrl+A select all, Ctrl+F search, Ctrl+L type a
location, Ctrl+H hidden files, Ctrl+1 list, Ctrl+2 icons, Ctrl+Shift+N new folder, Esc
select nothing.

**Text Editor**: Ctrl+S save, Ctrl+Q quit.

**Image Viewer**: `+` and `-` zoom, `0` fit.

## When something goes wrong

- **A program misbehaves**: Vexa stops it and prints why (a page fault, say, with the
  address), and everything else carries on. Ctrl-C stops a program that doesn't
  end.
- **The desktop stops responding**: Ctrl+Alt+Q goes back to the console, if the
  desktop still reads the keyboard.
- **Vexa doesn't boot on a machine**: try **safe mode** from the boot menu, then add
  `nosmp` in `limine.conf`.
- **Something is badly broken**: the **kernel monitor** entry in the boot menu starts
  the kernel's own command line (`help` lists its commands), without any programs.
- **The kernel stops** ("VEXA KERNEL PANIC"): the screen (and the serial port) show
  the reason and the CPU's registers. Please report it, with that text, on the
  [issues page](https://github.com/EnderiumCraft/Vexa/issues).
