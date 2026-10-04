#!/usr/bin/env python3
"""Boot build/vexa.iso in QEMU, type into the PS/2 keyboard, and check the serial log.

Usage: tools/qemu-smoke-test.py [--iso PATH] [--uefi] [--smp N] [--memory SIZE] [--cpu MODEL]
                                [--safe-mode] [--disks DIR] [--no-linux]

With --disks, attaches the test disks from `make test-disks` (copies, so the
originals stay pristine), runs file commands on them, and checks each with
e2fsck afterwards.

The guest gets a network card behind QEMU's user-mode NAT, and the test serves
a few files over HTTP on this machine (10.0.2.2 as the guest sees it) for the
guest to download.
                                [--screenshot out.png] [--keep-log]

Exits non-zero if an expected message is missing or the kernel panics.
"""
import argparse
import functools
import hashlib
import http.server
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

BOOT_TIMEOUT = 60
PROMPT = "vexa:"
OVMF = os.environ.get("OVMF", "/usr/share/qemu/OVMF.fd")

# Messages the kernel must print while booting, normally and in safe mode. Safe
# mode ignores ACPI, so there is no MADT and Vexa falls back to the 8259 PIC and
# the PIT, as it must on machines whose firmware has no MADT.
EXPECTED_BOOT_APIC = [
    "[acpi] revision",
    "[apic]",
    "[timer] local APIC timer",
    "[kbd] PS/2 keyboard ready",
    "Vexa kernel initialized",
]
EXPECTED_BOOT_LEGACY = [
    "[acpi] disabled by acpi=off",
    "[irq] using the legacy 8259 PIC",
    "[timer] PIT at 1000 Hz",
    "[kbd] PS/2 keyboard ready",
    "Vexa kernel initialized",
]

# Typed at the vsh prompt, with text that must appear in response (None: just
# wait for the prompt to come back), how many seconds to wait for it, and
# optionally how many times it must appear in the whole log. Typed text is
# echoed to the log too, which is why some counts are 2. "^C" presses Ctrl-C
# a second after the rest of the line was typed. A command starting with "@"
# goes to the QEMU monitor instead (to move the mouse, say), and "@type ..."
# is typed without waiting for vsh's prompt first. ("#name",) starts a
# section, which --only picks.
def move(x0, y0, x1, y1):
    """Mouse moves from (x0, y0) to (x1, y1), in steps QEMU takes whole."""
    dx, dy, steps = x1 - x0, y1 - y0, []
    while dx or dy:
        sx, sy = max(-250, min(250, dx)), max(-250, min(250, dy))
        steps.append((f"@mouse_move {sx} {sy}", None, 3))
        dx, dy = dx - sx, dy - sy
    return steps


def click(x0, y0, x1, y1, expected=None, timeout=15):
    """Moves there and clicks the left button."""
    return move(x0, y0, x1, y1) + [("@mouse_button 1", expected, timeout),
                                   ("@mouse_button 0", None, 3)]


TYPED_COMMANDS = ([
    ("#shell",),
    ("help", "run in the background", 10),
    ("hello", "hi :)", 10),
    ("uptime", "MiB memory free", 10),
    ("hostname vexa-test", "vexa-test", 10, 2),
    ("df", "iso9660", 10),
    ("ls /", "README.txt", 10),
    ("ls /bin", "vsh", 10, 2),
    ("cat /etc/motd", "Welcome to Vexa", 10),
    ("ln -s /etc/motd /tmp/motd-link ; cat /tmp/motd-link", "Welcome to Vexa", 10, 2),
    ("ls /proc", "meminfo", 10),
    # Native programs use libvexa.so through /lib/vexa-ld.so.
    ("ls /lib", "vexa-ld.so", 10),
    ("cat /proc/self/status", "Name:\tcat", 10),
    # Pipes, redirection and variables.
    ("echo one two | cat | cat", "one two", 10, 2),
    ("export WHAT=pipes", None, 10),
    ("echo $WHAT-work", "pipes-work", 10),
    ("echo redirected > /tmp/r.txt", None, 10),
    ("echo appended >> /tmp/r.txt", None, 10),
    ("cat < /tmp/r.txt | cat", "redirected\r\nappended", 10),
    # Files and directories.
    ("mkdir -p /tmp/a/b", None, 10),
    ("cp /etc/motd /tmp/a/b/copy", None, 10),
    ("mv /tmp/a/b/copy /tmp/a/moved", None, 10),
    ("ls -l /tmp/a", "moved", 10, 2),
    ("cd /tmp/a", "vexa:/tmp/a> ", 10),
    ("pwd", "/tmp/a\r\n", 10),
    ("cd ..", None, 10),
    ("rm -r a ; ls", "r.txt", 10),
    ("cd /", None, 10),
    ("fs-test", "fs-test: passed", 60),
    # The Phase 3 milestone: a native program in user mode.
    ("hello-world", "running in user mode.", 30),
    # A program that misbehaves is stopped, and the system carries on.
    ("crash", "kernel pointer rejected", 30),
    ("echo exit code $?", "exit code 139", 10),
    # Ctrl-C stops the foreground program, not the shell.
    ("sleep 30^C", None, 10),
    ("echo interrupted: $?", "interrupted: 130", 10),
    ("sleep 0.2 ; echo slept", "slept", 10, 2),
    # Three programs at once, each checking its vector registers survive.
    ("fpu-stress &", "started in the background", 10),
    ("fpu-stress &", "started in the background", 10, 2),
    ("fpu-stress", "): passed, 20 rounds", 300, 3),
    ("ps", "vinit", 10),
    # Priorities: nice runs a command with a higher nice value (it inherits
    # it); two busy programs, one niced, still both finish.
    ("nice -n 5 nice", "\n5\r\n", 10),
    ("nice -n 19 fpu-stress &", "started in the background", 10, 3),
    ("fpu-stress", "): passed, 20 rounds", 300, 5),
    # The kernel monitor's commands, through `sys`.
    # Threads: four sharing a mutex, then exiting with threads still running.
    ("thread-test", "thread-test: passed", 60),
    ("thread-test exit", "exiting with threads still running", 30),
    ("posix-test -v", "posix-test: passed", 180),  # (-v: which part it was on, if it hangs)
    # C++ (libc++ on libvexa, built with the SDK's vexa-c++); its static
    # object's destructor runs after main.
    ("cxx-test", "cxx-test: passed\r\ncxx-test: static destructor ran", 60),
    ("cxx-test-static", "cxx-test: passed\r\ncxx-test: static destructor ran", 60),
    ("sys mem", "heap ", 10),
    ("sys threads", "idle", 10),
    ("sys memtest", "memtest: passed", 180),
    ("#network",),
    # Networking: DHCP, sockets over loopback, and a download from this machine
    # (@URL@ is the test's HTTP server).
    ("net", "address 10.0.2.15", 10),
    ("socket-test", "socket-test: passed", 60),
    # IPv6: a link-local address, then QEMU's router advertises fec0::/64
    # (an address from it, and a default route); fec0::2 answers pings.
    ("net", "router6 fe80::2", 10),
    ("net", "address6 fec0::5054:ff:fe12:3456/64", 10),
    ("fetch @URL@/hello.txt", "Hello from the test's web server", 30),
    # HTTPS (Mbed TLS): a server with the test's own certificate authority is
    # refused, then trusted with it.
    ("fetch -o /tmp/test-ca.pem @URL@/test-ca.pem", "saved", 30),
    ("fetch @HTTPS@/hello.txt", "certificate isn't trusted", 60),
    ("fetch --ca /tmp/test-ca.pem @HTTPS@/hello.txt", "Hello from the test's web server", 60, 2),
    ("fetch --ca /tmp/test-ca.pem -o /tmp/https.bin @HTTPS@/data.bin", "saved 1048576 bytes", 180),
    ("#sound",),
    # Sound through the HD Audio driver: QEMU records what's played, and the
    # recording should be the tone.
    ("play --tone 1000 2", "play: done", 60),
    ("@sound 1000", None, 10),
    ("#desktop",),
    # Graphics: the mouse, then the desktop with a terminal window. Typing
    # goes to the window's shell; dragging its title bar moves it (the pointer
    # starts in the middle of the 1280x800 screen; QEMU can drop part of a
    # long move, so those are split); Ctrl+Alt+T opens another; Ctrl+Alt+Q
    # goes back.
    # Apps are .vxapp bundles in /apps; their programs are linked in /bin.
    ("ls /apps", "Terminal.vxapp", 10),
    ("ls -l /bin", "files -> /apps/Files.vxapp/Contents/Vexa/files", 10),
    # cp -r and rm -r, on a bundle.
    ("cp -r /apps/About.vxapp /tmp/About.vxapp", None, 5),
    ("ls /tmp/About.vxapp/Contents", "Resources", 10),
    ("rm -r /tmp/About.vxapp", None, 5),
    ("ls /tmp/About.vxapp", "ls: /tmp/About.vxapp: ", 10),
    ("input", "PS/2 mouse", 10),
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@type echo from-the-window > /dev/console", "from-the-window", 30),
    ("@mouse_move -440 -330", None, 5),
    ("@mouse_button 1", "desktop: left button at 200,70", 10),
    ("@mouse_move 100 50", None, 5),
    ("@mouse_button 0", "desktop: moved window 1 to 180,132", 10),
    # Its bottom right corner (828,524) resizes it: 100 by 50 pixels bigger,
    # which the terminal rounds to whole characters.
    ("@mouse_move 264 202", None, 5),
    ("@mouse_move 264 202", None, 5),
    ("@mouse_button 1", "desktop: left button at 828,524", 10),
    ("@mouse_move 100 50", None, 5),
    ("@mouse_button 0", "desktop: window 1 is now 744x440", 10),
    ("@sendkey ctrl-alt-t", "desktop: window 2", 30),
    ("@type exit", "desktop: closed window 2", 30),
    # The Vexa menu (top left) starts "About Vexa"; its close button closes it.
    ("@mouse_move -299 -187", None, 5),
    ("@mouse_move -299 -187", None, 5),
    ("@mouse_move -299 -187", None, 5),
    ("@mouse_button 1", "desktop: left button at 31,13", 10),
    ("@mouse_button 0", None, 5),
    ("@mouse_move 0 125", None, 5),
    ("@mouse_button 1", 'desktop: window 3 "About Vexa"', 20),
    ("@mouse_button 0", None, 5),
    ("@mouse_move 463 -11", None, 5),
    ("@mouse_button 1", "desktop: asked window 3 to close", 10),
    ("@mouse_button 0", "desktop: closed window 3", 10),
    # Notifications, and the image viewer (PNG) from the terminal, through
    # open: the app for .png files (from the .vxapp bundles in /apps).
    ("@type notify Test: a notification", 'desktop: notification "Test: a notification"', 20),
    ("@type open /share/pictures/aurora.png", 'desktop: window 4 "aurora.png - Image Viewer"', 30),
    # A double click on the Files icon (on the desktop's left edge).
    ("@mouse_move -454 23", None, 5),
    ("@double-click", "desktop: starting Files", 10),
    ("@mouse_move 0 0", 'desktop: window 5 is now called "/ - Files"', 30),
    # Files (at 208,194, with the keyboard): to /apps (Ctrl+L, a path), copy an
    # app and paste it (the desktop notices a new app), rename it, Get Info,
    # Quick Look, Duplicate, Move to Trash (both: the app is gone again), up.
    ("@sendkey ctrl-l", None, 2),
    ("@type /apps", 'desktop: window 5 is now called "/apps - Files"', 10),
    ("@sendkey down", None, 2),
    ("@sendkey ctrl-c", "files: copied /apps/About.vxapp", 10),
    ("@sendkey ctrl-v", "files: pasted /apps/About.vxapp to /apps/About 2.vxapp", 10),
    ("@mouse_move 0 0", "desktop: apps changed", 10),
    ("@sendkey f2", None, 2),
] + [("@sendkey backspace", None, 1)] * 13 + [
    ("@type Hello.vxapp", "files: renamed /apps/About 2.vxapp to /apps/Hello.vxapp", 10),
    ("@sendkey ctrl-i", "files: info for Hello.vxapp: App", 10),
    ("@sendkey esc", None, 2),
    ("@sendkey spc", "files: quick look at Hello.vxapp", 10),
    ("@sendkey esc", None, 2),
    ("@sendkey ctrl-d", "files: pasted /apps/Hello.vxapp to /apps/Hello 2.vxapp", 10),
    ("@sendkey delete", "files: moved /apps/Hello 2.vxapp to the Trash", 10),
    ("@sendkey delete", "files: moved /apps/Hello.vxapp to the Trash", 10),
    ("@sendkey backspace", 'desktop: window 5 is now called "/ - Files"', 10),
    # Right clicks: in Files' list (its menu), then on the desktop, whose
    # menu starts "About Vexa"; then the pointer goes back to the Files icon.
    ("@mouse_move 300 221", None, 5),
    ("@mouse_move 368 263", None, 5),
    ("@mouse_button 2", "files: menu for /", 10),
    ("@mouse_button 0", None, 2),
    ("@sendkey esc", None, 2),
    ("@mouse_move 292 16", None, 5),
    ("@mouse_button 2", "desktop: menu at 1000,650", 10),
    ("@mouse_button 0", None, 2),
    ("@mouse_move 20 112", None, 5),
    ("@mouse_button 1", 'desktop: menu item "About Vexa"', 10),
    ("@mouse_button 0", 'desktop: window 6 "About Vexa"', 20),
    ] + move(1020, 762, 40, 150) + [
    # Settings (from the Vexa menu; window 7, at 272,49): the dark theme
    # (every window changes), light again, a time zone, and 1024x768 (which
    # goes back by itself after 15 seconds), then closed.
    ] + click(40, 150, 31, 13, "desktop: left button at 31,13") + [
    ("@mouse_move 0 100", None, 3),
    ("@mouse_button 1", None, 3),
    ("@mouse_button 0", 'desktop: window 7 "Settings"', 20),
    ] + click(31, 113, 576, 190, "desktop: settings reloaded (dark, blue)", 20)
      + click(576, 190, 756, 190, "desktop: settings reloaded (light, blue)", 20)
      + click(756, 190, 372, 218, "settings: showing Date & Time")
      + click(372, 218, 706, 392, "settings: time_zone=Denver")
      + click(706, 392, 372, 286, "settings: showing Display")
      + click(372, 286, 769, 154, "desktop: display now 1024x768", 30) + [
    ("@mouse_move 0 0", "settings: display=back as it was", 40),
    ("@mouse_move 0 0", "desktop: display now 1280x800", 30),
    ] + click(769, 154, 1122, 38, "desktop: asked window 7 to close")
      + move(1122, 38, 40, 150) + [
    # Dragging the terminal by its title bar to the left edge: half the screen.
    ("@mouse_move 280 -15", None, 5),
    ("@mouse_move 280 -15", None, 5),
    ("@mouse_button 1", "desktop: left button at 600,120", 10),
    ("@mouse_move -300 0", None, 5),
    ("@mouse_move -300 0", None, 5),
    ("@mouse_button 0", "desktop: snapped window 1 to the left", 10),
    ("@mouse_move 1 0", "desktop: window 1 is now 632x744", 20),
    # A file in the Desktop folder is an icon on the desktop; Super+arrows
    # snap (the left half, then its top quarter, then the top right one);
    # Alt+Tab switches; Ctrl+Space searches (and does sums); PrintScreen
    # saves a screenshot in Pictures; Super+L locks, and Enter unlocks.
    ("@type echo hi > /home/Desktop/note.txt", "desktop: desktop folder changed", 20),
    ("@sendkey meta_l-up", "desktop: snapped window 1 to the top left", 10),
    ("@sendkey meta_l-right", "desktop: snapped window 1 to the top right", 10),
    ("@sendkey alt-tab", "desktop: switched to window", 10),
    ("@sendkey ctrl-spc", "desktop: search", 10),
    ("@type 12*(3+4)", 'desktop: notification "Calculator: 12*(3+4) = 84"', 10),
    ("@sendkey print", "desktop: screenshot 1280x800 saved to /home/Pictures/Screenshot", 60),
    ("@sendkey meta_l-l", "desktop: locked", 10),
    ("@sendkey ret", "desktop: unlocked", 10),
    # The apps: a second tab in the terminal (exit closes it); the
    # Calculator (from search) does a sum; Notes makes a note.
    ("@sendkey alt-tab", "desktop: switched to window", 10),
    ("@sendkey ctrl-alt-t", "term: tab 1 of 1", 20),
    ("@sendkey ctrl-shift-t", "term: tab 2 of 2", 20),
    ("@type exit", None, 5),
    ("@sendkey ctrl-spc", "desktop: search", 10),
    ("@type Calculator", '"Calculator" (300x420)', 20),
    ("@type 12*3=", "calc: 12 \u00d7 3 = 36", 10),
    ("@sendkey alt-f4", "desktop: asked window", 10),
    ("@sendkey ctrl-spc", "desktop: search", 10),
    ("@type Notes", '"Notes" (780x520)', 20),
    ("@sendkey ctrl-n", "notes: new note Note.txt", 10),
    ("@sendkey alt-f4", "desktop: asked window", 10),
    # Device Manager: the devices by type; Down picks the first one; Tab shows
    # them as they're connected.
    ("@sendkey ctrl-spc", "desktop: search", 10),
    ("@type Device Manager", '"Device Manager" (780x520)', 20),
    ("@sendkey down", 'devmgr: showing "', 10),
    ("@sendkey tab", None, 2),
    ("@sendkey alt-f4", "desktop: asked window", 10),
    # Doom (Chocolate Doom, with Freedoom's levels from the boot CD): its
    # title music (SDL_mixer's OPL synthesizer), a new game from its menu,
    # and quitting (Alt+F4 asks the game, which asks the player).
    ("@sendkey ctrl-spc", "desktop: search", 10),
    ("@type Doom", '"Freedoom: Phase 1 - Chocolate Doom 3.1.0" (800x600)', 60),
    ("@music", None, 20),
    ("@sendkey esc", None, 3),
    ("@sendkey ret", None, 3),
    ("@sendkey ret", None, 3),
    ("@sendkey ret", None, 3),
    ("@sendkey alt-f4", "desktop: asked window", 10),
    ("@sendkey y", 'desktop: closed window', 30),
    # Its first demo, played as fast as it goes (without drawing it, or sound:
    # the SDL demo's chime is listened for later).
    ("@sendkey ctrl-alt-t", "term: tab 1 of 1", 20, 4),
    ("@type chocolate-doom -timedemo demo1 -nodraw -nosound -nogui > /dev/console 2> /dev/console",
     "gametics in", 600),
    # OpenGL in a native program: SDL's window with a context (Mesa's
    # softpipe, loaded from the boot CD), checked with glReadPixels.
    ("@type sdl-gl-test > /dev/console 2> /dev/console", "sdl-gl-test: passed", 300),
    # The web browser (NetSurf, a native app): a page from the test's server
    # (its title becomes the window's), then Alt+F4 closes it.
    ("@type netsurf @URL@/page.html > /dev/console 2> /dev/console &",
     'is now called "Vexa test page - NetSurf"', 180),
    ("@sendkey alt-f4", "desktop: closed window", 30),
    # An error (no such WAD) shows in a window (SDL's message box), which
    # Enter closes.
    ("@type chocolate-doom -iwad missing.wad", '"Chocolate Doom 3.1.0" (460x', 60),
    ("@sendkey ret", "desktop: closed window", 20),
    # A network game (SDL_net, over libvexa's sockets) on this machine: a
    # server with a player, and another player joining it; then both quit.
    ("@type chocolate-doom -server -nodes 2 -nosound -nogui > /dev/console 2> /dev/console &",
     "NET_Init", 120),
    ("@sendkey ctrl-alt-t", None, 10),
    ("@type chocolate-doom -connect 127.0.0.1 -nosound -nogui > /dev/console 2> /dev/console",
     "player 2 of 2 (2 nodes)", 300),
    ("@sendkey shift", "player 1 of 2 (2 nodes)", 120),
    ("@sendkey alt-f4", None, 5),
    ("@sendkey y", None, 20),
    ("@sendkey alt-f4", None, 5),
    ("@sendkey y", None, 20),
    ("@type exit", None, 5),
    ("@type exit", None, 5),
    ("@sendkey ctrl-alt-q", "desktop: asking before leaving", 20),
    ("@sendkey ret", "desktop: back to the console", 20),
    # (Keys typed while the desktop ran can be left at the console: Ctrl-U erases them.)
    ("@sendkey ctrl-u", None, 2),
    ("ls /home/Desktop", "note.txt", 10),
    ("ls /home/Pictures", "Screenshot 20", 10),
    ("#shell",),
    ("Hello Vexa", "Hello: command not found", 10),
])

# The Linux subsystem: BusyBox (built from source with musl) from vsh, then
# its shell, with fork, exec, pipes, Ctrl-C and signal handlers. Typed before
# "ps" unless --no-linux (for kernels built with LINUX_COMPAT=0).
TYPE_ATTEMPTS = 3  # For "@type" lines (see main).

LINUX_COMMANDS = [
    ("#linux",),
    ("busybox echo hello from linux", "hello from linux", 20, 2),
    ("busybox uname -sr", "Vexa 6.1.0-vexa", 20),
    ("busybox nice -n 3 busybox nice", "\n3\r\n", 20),
    ("busybox uname -n", "vexa-test", 20, 3),  # The name `hostname` gave it.
    ("busybox sh -c 'echo answer $((6*7))'", "answer 42", 20),
    ("export PS1='\\166exa:bb# '", None, 10),  # "vexa:bb# ", so prompts are counted
    ("busybox sh", "vexa:bb# ", 20),
    ("cd /tmp && echo made > bb.txt && cat bb.txt", "made\r\n", 20),
    ("seq 5 7 | tr '\\n' +", "5+6+7+", 20),
    ("sleep 30^C", None, 20),
    ("echo after ctrl-c $?", "after ctrl-c 130", 20),
    ("sh -c 'kill -TERM $$' ; echo killed $?", "killed 143", 20),
    ("trap 'echo caught' USR1 ; kill -USR1 $$", "caught", 20, 2),
    ("for i in 1 2 3 4 5 ; do echo n$i ; done | wc -l | sed s/^/lines:/", "lines:5", 20),
    ("/bin/hello", "hi :)", 20, 2),  # A native program, started by a Linux one.
    ("exit", None, 20),
    # Dynamically linked programs (musl's loader), /proc, scripts, bash.
    ("busybox free", "Swap:", 20),
    ("busybox printf '#!/bin/sh\\necho script says $1\\n' > /tmp/s.sh", None, 20),
    ("chmod +x /tmp/s.sh ; /tmp/s.sh hi", "script says hi", 20),
    ("bash -c 'echo bash $BASH_VERSION'", "bash 5.2", 20),
    ("bash -c 'f() { echo fn $1 ; } ; a=(x y z) ; f ${a[2]}'", "fn z", 20),
    ("bash -c 'cat <(echo substituted) | wc -c | sed s/^/count:/'", "count:12", 20),
    # GNU coreutils; POSIX timers (timeout), statfs (df), permissions.
    ("/linux/bin/ls --version", "(GNU coreutils)", 20),
    ("bash -c 'timeout 1 sleep 5 ; echo timeout-status $?'", "timeout-status 124", 20),
    ("df /", "tmpfs", 20),
    # Python 3: threads, subprocess, multiprocessing, shared memory, signals...
    ("python-test.py", "python-test: passed", 300),
    # Linux threads (musl's pthreads): clone, futex, thread-local storage, tgkill.
    ("pthread-test", "pthread-test: passed", 60),
    ("pthread-test exit", "exiting while threads spin", 30),
    # memfd_create (files in memory, mapped shared) and eventfd.
    ("memfd-test", "memfd-test: passed", 30),
    ("#x",),
    # X: the desktop, then an xterm (Ctrl+Alt+X) through Xvexa, a rootless X
    # server: the xterm is a desktop window, which has the keyboard, and its
    # close button (736,88 to 756,110) closes it (WM_DELETE_WINDOW).
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@sendkey ctrl-alt-x", 'desktop: window 2 "xterm"', 90),
    ("@type echo typed-in-xterm > /dev/console", "typed-in-xterm", 20),
    # The clipboard every app shares: what's written to /run/clipboard (here
    # by the xterm's shell) pastes in X programs (Shift+Insert: PRIMARY,
    # which xclipboard owns too).
    ("@type printf 'echo pasted-%s-in-xterm > /dev/console' 42 > /run/clipboard", None, 3),
    ("@sendkey shift-insert", None, 3),
    ("@sendkey ret", "pasted-42-in-xterm", 20),
    ("@mouse_move 53 -150", None, 5),
    ("@mouse_move 53 -150", None, 5),
    ("@mouse_button 1", "desktop: asked window 2 to close", 10),
    ("@mouse_button 0", 'desktop: closed window 2 "xterm"', 30),
    # GTK 3: gtk3-demo from the Vexa menu (below the native apps; at 144,138, with GTK's own title
    # bar); a click on "Change Display" in its list shows that demo, which is
    # its title then.
    ("@mouse_move -357 -43", None, 5),
    ("@mouse_move -358 -44", None, 5),
    ("@mouse_button 1", "desktop: left button at 31,13", 10),
    ("@mouse_button 0", None, 5),
    ("@mouse_move 0 199", None, 5),
    ("@mouse_move 0 223", None, 5),
    ("@mouse_button 1", "desktop: left button at 31,435", 10),
    ("@mouse_button 0", "desktop: window 3", 300),
    ("@mouse_move 181 -127", None, 10),
    ("@mouse_button 1", "desktop: left button at 212,308", 10),
    ("@mouse_button 0", 'desktop: window 3 is now called "Change Display"', 60),
    # GTK draws its own title bar (the desktop draws none for it); its
    # maximize button asks the desktop, through Xvexa, like a window manager.
    ("@mouse_move 337 -72", None, 5),
    ("@mouse_move 338 -72", None, 5),
    ("@mouse_button 1", "desktop: left button at 887,164", 10),
    ("@mouse_button 0", "desktop: maximized window 3", 30),
    # OpenGL in an X window: Mesa's llvmpipe through GLX, from another xterm
    # (the GL window is window 5, and closes itself).
    ("@sendkey ctrl-alt-x", 'desktop: window 4 "xterm"', 90),
    ("@type gl-test x > /dev/console 2>&1", "gl-test: passed", 180),
    # The D-Bus session bus xrun started for X programs: the bus itself
    # answers with the names on it.
    ("@type dbus-send --session --print-reply --dest=org.freedesktop.DBus "
     "/org/freedesktop/DBus org.freedesktop.DBus.ListNames > /dev/console 2>&1",
     'string "org.freedesktop.DBus"', 60),
    ("@type exit", 'desktop: closed window 4 "xterm"', 30),
    ("@sendkey ctrl-alt-q", "desktop: asking before leaving", 30),
    ("@sendkey ret", "desktop: back to the console", 30),
    ("#linux-net",),
    # Networking: BSD sockets (with SCM_RIGHTS), wget, ifconfig, ping and
    # Python's urllib, asyncio and multiprocessing pipes.
    ("bsd-socket-test", "bsd-socket-test: passed", 60),
    ("wget -q -O /tmp/data.bin @URL@/data.bin", None, 60),
    ("sha1sum /tmp/data.bin", "@SHA1@", 30),
    ("ifconfig eth0", "inet addr:10.0.2.15", 20),
    ("ping -c 2 127.0.0.1", "2 packets received", 30),
    # IPv6 (raw ICMPv6 sockets): loopback, then QEMU's router over the wire.
    ("ping6 -c 2 ::1", "2 packets received", 30),
    ("ping6 -c 2 fec0::2", "2 packets received", 30),
    ("cat /proc/net/if_inet6", "fec0000000000000505400fffe123456 02 40 00 80 eth0", 10),
    ("python3 -c \"import socket; s = socket.create_server(('::', 0), family=socket.AF_INET6, "
     "dualstack_ipv6=True); p = s.getsockname()[1]; socket.create_connection(('::1', p)); "
     "socket.create_connection(('127.0.0.1', p)); print('from', s.accept()[1][0], 'and', "
     "s.accept()[1][0])\"", "from ::1 and ::ffff:127.0.0.1", 60),
    ("python-net-test.py @URL@/data.bin @SHA1@", "python-net-test: passed", 300),
    # HTTPS: OpenSSL, curl and Python's ssl module, with the root certificates
    # (and, for the test's own server, its certificate authority: without it,
    # curl refuses the server). Random numbers from the kernel's generator.
    ("openssl version", "OpenSSL 3.0", 30),
    ("python3 -c \"import os; print(len(set(os.urandom(16) for i in range(500))), 'different')\"",
     "500 different", 60),
    ("wget -q -O /tmp/test-ca.pem @URL@/test-ca.pem", None, 30),
    ("curl -sS @HTTPS@/hello.txt", "SSL certificate problem", 60),
    ("curl -sS --cacert /tmp/test-ca.pem @HTTPS@/hello.txt",
     "Hello from the test's web server", 60),
    ("curl -sS --cacert /tmp/test-ca.pem -o /tmp/data2.bin @HTTPS@/data.bin", None, 120),
    ("sha1sum /tmp/data2.bin", "@SHA1@", 30),
    ("python3 -c \"import ssl; print(ssl.create_default_context().cert_store_stats())\"",
     "'x509_ca': 1", 60),
    ("python3 -c \"import ssl, urllib.request as u; c = ssl.create_default_context("
     "cafile='/tmp/test-ca.pem'); print(u.urlopen('@HTTPS@/hello.txt', context=c).read())\"",
     "b\"Hello from the test's web server", 120),
    ("#linux-graphics",),
    # Linux's graphics and input interfaces: a DRM program shows pictures on
    # the whole screen (taking it from the console, then giving it back) and
    # reads the grabbed keyboard through evdev.
    ("drm-test", "drm-test: press a key", 60),
    ("@sendkey a", "drm-test: passed", 30),
    # Xorg on the whole screen (modesetting over DRM, evdev input), with an
    # xterm as its client; when the client ends, Xorg does too.
    ("startxorg xterm -e sh -c 'echo xorg client on $DISPLAY > /dev/console'",
     "xorg client on :1", 180),
    ("grep -c 'modeset(0)' /tmp/Xorg.1.log", None, 10),
    ("grep evdev /tmp/Xorg.1.log | head -3", "evdev", 10),
    ("#linux-sound",),
    # Sound for Linux programs: ALSA (alsa-lib's "plug" on the kernel's ALSA
    # interface, over the HD Audio driver). Python writes a WAV file, aplay
    # plays it, and QEMU's recording should be its tone.
    ("python3 -c \"import wave, math, struct; w = wave.open('/tmp/tone.wav', 'wb'); "
     "w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100); "
     "w.writeframes(b''.join(struct.pack('<h', int(9000 * math.sin(2 * math.pi * 600 * i / 44100))) "
     "for i in range(88200)))\"", None, 120),
    ("aplay /tmp/tone.wav && echo aplay-finished", "aplay-finished", 60),
    ("@sound 600", None, 10),
    # OpenGL without X: Mesa's llvmpipe (shaders compiled by LLVM) into memory.
    ("gl-test", "gl-test: passed", 180),
    ("#shell",),
]

# With --disks: one ext2 file system on each kind of disk.
# With --usb: an xHCI controller with a keyboard, and a hub with a mouse on
# it. QEMU sends keys and mouse motion to the newest keyboard and mouse, so
# then everything typed and pointed goes through USB.
# --usb CONTROLLER: a keyboard, and a hub with a mouse on it. With ehci, an
# Intel ICH9's USB 2 controller and its three USB 1 companions: the keyboard
# is high speed (EHCI's), the hub full speed (handed to a UHCI controller).
USB_DEVICES = {
    "xhci": ["-device", "qemu-xhci,id=usb"],
    "ehci": ["-device", "ich9-usb-ehci1,id=usb,addr=1d.7,multifunction=on",
             "-device", "ich9-usb-uhci1,masterbus=usb.0,firstport=0,addr=1d.0,multifunction=on",
             "-device", "ich9-usb-uhci2,masterbus=usb.0,firstport=2,addr=1d.1,multifunction=on",
             "-device", "ich9-usb-uhci3,masterbus=usb.0,firstport=4,addr=1d.2,multifunction=on"],
    "uhci": ["-device", "piix3-usb-uhci,id=usb"],
    "ohci": ["-device", "pci-ohci,id=usb"],
}
USB_ATTACHED = [
    "-device", "usb-kbd,bus=usb.0,port=1",
    "-device", "usb-hub,bus=usb.0,port=2",
    "-device", "usb-mouse,bus=usb.0,port=2.1",
]

# With --usb, at the end of the desktop checks: Device Manager open while a
# USB stick is plugged in and pulled out; the desktop says so too.
USB_DESKTOP_COMMANDS = [
    ("@sendkey ctrl-spc", "desktop: search", 10),
    ("@type Device Manager", '"Device Manager" (780x520)', 20, 2),
    ("@drive_add 0 if=none,id=stick3,file=@STICK@,format=raw", None, 5),
    ("@device_add usb-storage,bus=usb.0,drive=stick3,id=stick3", "devmgr: devices changed", 30),
    ("@mouse_move 0 0", 'desktop: notification "Connected: QEMU USB HARDDRIVE', 20),
    ("@device_del stick3", 'desktop: notification "Disconnected: QEMU USB HARDDRIVE"', 30),
    ("@sendkey alt-f4", "desktop: asked window", 10),
    # A tablet (absolute positions). QEMU's monitor only moves pointers
    # relatively, which a tablet ignores: it stays at 0,0, so its click puts
    # the pointer there (from wherever the mouse left it).
    ("@device_add usb-tablet,bus=usb.0,id=tablet", "tablet (absolute)", 20),
    ("@mouse_button 1", "desktop: left button at 0,0", 10),
    ("@mouse_button 0", None, 2),
    ("@device_del tablet", "QEMU USB Tablet unplugged", 20),
]

USB_COMMANDS = [
    ("#usb",),
    # (Everything was found while the desktop was up, at the start.)
    ("devices usb", "QEMU USB Mouse", 10),
    ("devices usb", "QEMU USB Keyboard  [usb-hid]", 10),
    ("devices -l usb", "port 1 of a hub", 10),
    ("devices keyboard", "PS/2 keyboard", 10),
    # A USB stick plugged in while Vexa runs: mounted at /mnt/usb0, read and
    # written; pulled out (its file system goes), and plugged in again.
    ("@drive_add 0 if=none,id=stick,file=@STICK@,format=raw", None, 5),
    ("@device_add usb-storage,bus=usb.0,drive=stick,id=stick", "mounted usb0 at /mnt/usb0", 30),
    ("cat /mnt/usb0/hello.txt", "Hello from a USB stick!", 10),
    ("echo written over usb > /mnt/usb0/note.txt", None, 10),
    ("devices -l disk", "usb0 at /mnt/usb0", 10),
    ("@device_del stick", "usb0 is gone", 20),
    ("ls /mnt/usb0", "vexa:/> ", 10),
    ("df", "cd0", 10),
    ("@drive_add 0 if=none,id=stick2,file=@STICK@,format=raw", None, 5),
    ("@device_add usb-storage,bus=usb.0,drive=stick2,id=stick2", "mounted usb0 at /mnt/usb0", 30, 2),
    ("cat /mnt/usb0/note.txt", "written over usb\r\n", 10),
    ("@device_del stick2", "usb0 is gone", 20, 2),  # (QEMU opens an image only once.)
    # A second keyboard, plugged in and out.
    ("@device_add usb-kbd,bus=usb.0,id=kbd2", "usb-hid] QEMU USB Keyboard: keyboard", 20),
    ("@device_del kbd2", "QEMU USB Keyboard unplugged", 20),
    ("echo still typing", "still typing\r\n", 10),
]

# On the test disks (so only with --disks): apps built with the SDK (make
# sdk-test), opened from a new terminal at the end of the desktop checks.
SDK_APP_COMMANDS = [
    ("@sendkey ctrl-alt-t", "term: tab 1 of 1", 20),
    ("@type open /mnt/vda1/HelloSDK.vxapp", '"Hello SDK" (420x220)', 20),
    ("@sendkey alt-f4", "desktop: asked window", 10),
    # The SDL demo (SDL 2 with Vexa's drivers): a window drawn with SDL's
    # renderer, its chime through SDL's audio, Space (pauses: a new title)
    # and Escape (quits) through SDL's events.
    ("@type open /mnt/vda1/SDLDemo.vxapp", '"SDL Demo" (640x400)', 30),
    ("@sound 660", None, 10),
    ("@sendkey spc", 'is now called "SDL Demo (paused)"', 60),  # (Slow in safe mode.)
    ("@sendkey esc", "desktop: closed window", 10),
    ("@type exit", None, 5),
]

# With --virgl: the virtio GPU with 3D (QEMU's virgl, on this machine's
# OpenGL), and OpenGL on it in a native program (Mesa's virgl driver).
VIRGL_COMMANDS = [
    ("#virgl",),
    ("ls /dev/dri", "renderD128", 10),
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@type sdl-gl-test > /dev/console 2> /dev/console", "sdl-gl-test: virgl", 120),
    ("@mouse_move 0 0", "sdl-gl-test: passed", 120),
    # VEXA_GL=softpipe: Mesa on the processor even so.
    ("@type export VEXA_GL=softpipe", None, 5),
    ("@type sdl-gl-test > /dev/console 2> /dev/console", "sdl-gl-test: softpipe", 120),
    ("@mouse_move 0 0", "sdl-gl-test: passed", 120),
    ("@sendkey ctrl-alt-q", "desktop: asking before leaving", 20),
    ("@sendkey ret", "desktop: back to the console", 20),
]

DISK_COMMANDS = [
    ("#disks",),
    ("sys disks", "nvme0n1", 10),
    ("sys mount", "/mnt/nvme0n1", 10),
    ("cat /mnt/vda1/hello.txt", "Hello from an ext2 disk!", 10),
    ("cat /mnt/sda1/docs/notes.txt", "lives on a test disk", 10),
    ("/mnt/nvme0n1/hello-world", "Hello, world!", 30, 2),
    ("fs-test", "on the disk at /mnt/vda1", 120, 2),
    ("echo written on sata > /mnt/sda1/from-vexa.txt", None, 10),
    ("cat /mnt/sda1/from-vexa.txt", "written on sata", 10, 2),
    ("mkdir /mnt/nvme0n1/made-by-vexa", None, 10),
    ("echo written on nvme > /mnt/nvme0n1/made-by-vexa/note.txt", None, 10),
    ("cat /mnt/nvme0n1/made-by-vexa/note.txt", "written on nvme", 10, 2),
    ("mv /mnt/nvme0n1/made-by-vexa/note.txt /mnt/nvme0n1/moved.txt", None, 10),
    ("cat /mnt/nvme0n1/moved.txt", "written on nvme", 10, 3),
    ("rm -r /mnt/nvme0n1/made-by-vexa", None, 10),
    # ext4, as Linux's mkfs.ext4 makes it: read (through extent trees, one
    # with an index block; a hashed directory), not written.
    ("cat /mnt/vdb/hello.txt", "Hello from an ext4 disk!", 10),
    ("cat /mnt/vdb/link", "Hello from an ext4 disk!", 10, 2),
    ("cat /mnt/vdb/dir/many/file-with-a-longer-name-399.txt", "\n399\r\n", 10),
    ("busybox sha1sum /mnt/vdb/sparse.bin", "66ae21c2cd4afeb16d02809402927d82edd153b9", 30),
    ("echo no > /mnt/vdb/new.txt", "read-only", 10),
    ("#shell",),
]

# (file name in the disks directory, QEMU arguments, where the ext2 starts)
TEST_DISKS = [
    ("virtio-gpt.img", ["-drive", "file={},if=virtio,format=raw"], 1024 * 1024),
    ("sata-mbr.img", ["-drive", "file={},if=none,id=sata0,format=raw",
                      "-device", "ide-hd,drive=sata0,bus=ide.0"], 1024 * 1024),
    ("nvme-whole.img", ["-drive", "file={},if=none,id=nvme0,format=raw",
                        "-device", "nvme,serial=vexa0,drive=nvme0"], 0),
    ("ext4.img", ["-drive", "file={},if=virtio,format=raw"], 0),
]


def check_filesystem(image, offset, tmp):
    """Runs e2fsck (read-only) on the ext2 file system inside a disk image."""
    target = image
    if offset:
        target = os.path.join(tmp, os.path.basename(image) + ".fs")
        with open(image, "rb") as src, open(target, "wb") as dst:
            src.seek(offset)
            dst.write(src.read())
    result = subprocess.run(["e2fsck", "-fn", target], capture_output=True, text=True)
    return result.returncode == 0, result.stdout + result.stderr

# QEMU `sendkey` names for characters that aren't plain lowercase letters or digits.
KEY_NAMES = {" ": "spc", "\n": "ret", "\b": "backspace", "-": "minus", ".": "dot", "/": "slash",
             ":": "shift-semicolon", "_": "shift-minus", ";": "semicolon", "=": "equal",
             "|": "shift-backslash", ">": "shift-dot", "<": "shift-comma", "$": "shift-4",
             "?": "shift-slash", "&": "shift-7", "\"": "shift-apostrophe", "'": "apostrophe",
             "(": "shift-9", ")": "shift-0", "*": "shift-8", "+": "shift-equal", "!": "shift-1",
             "#": "shift-3", "%": "shift-5", "\\": "backslash", ",": "comma", "@": "shift-2", "^": "shift-6",
             "[": "bracket_left", "]": "bracket_right", "\x1b": "esc",
             "{": "shift-bracket_left", "}": "shift-bracket_right", "~": "shift-grave_accent",
             "`": "grave_accent"}


def keys_for(text):
    for ch in text:
        if ch.isupper():
            yield "shift-" + ch.lower()
        else:
            yield KEY_NAMES.get(ch, ch)


class Monitor:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.sock.settimeout(2)
        self._drain()

    def _drain(self):
        time.sleep(0.2)
        try:
            self.sock.recv(65536)
        except socket.timeout:
            pass

    def command(self, cmd):
        self.sock.sendall(cmd.encode() + b"\n")
        self._drain()


def wait_for(log_path, text, timeout, count=1, start=0):
    """Waits for `text` to be in the log `count` times (after `start`)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        log = read_log(log_path)
        if "VEXA KERNEL PANIC" in log:
            return False
        if log[start:].count(text) >= count:
            return True
        time.sleep(0.2)
    return False


def read_log(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def ppm_to_png(ppm_path, png_path):
    data = open(ppm_path, "rb").read()
    header = data.split(b"\n", 3)
    width, height = map(int, header[1].split())
    pixels = header[3]
    raw = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body)))

    with open(png_path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw)))
        f.write(chunk(b"IEND", b""))


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def tiny_png(width, height, rgb):
    """A PNG of one color."""
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    rows = b"".join(b"\0" + bytes(rgb) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def start_web_server(directory):
    """Serves `directory` on a free port of this machine; returns the URL the
    guest uses for it, and the SHA-1 of data.bin."""
    with open(os.path.join(directory, "hello.txt"), "w") as f:
        f.write("Hello from the test's web server\n")
    # A page for the web browser: a title, style and a picture.
    with open(os.path.join(directory, "page.html"), "w") as f:
        f.write("<!DOCTYPE html><html><head><title>Vexa test page</title>"
                "<style>body { font-family: sans-serif; background: #eef; } "
                "h1 { color: #336; }</style></head><body><h1>Hello from the test</h1>"
                "<p>A paragraph, a <a href='hello.txt'>link</a>, and a picture:</p>"
                "<img src='dot.png' width='64' height='64'></body></html>\n")
    with open(os.path.join(directory, "dot.png"), "wb") as f:
        f.write(tiny_png(16, 16, (60, 140, 230)))
    # A megabyte that isn't all the same byte, so corruption would show.
    data = b"".join(hashlib.sha256(str(i).encode()).digest() for i in range(32768))
    with open(os.path.join(directory, "data.bin"), "wb") as f:
        f.write(data)
    handler = functools.partial(QuietHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    # QEMU's user-mode network shows this machine to the guest as 10.0.2.2.
    return "http://10.0.2.2:%d" % server.server_address[1], hashlib.sha1(data).hexdigest()


def start_https_server(directory):
    """Serves `directory` over HTTPS too, with a certificate for 10.0.2.2 from
    a certificate authority made for the test (its certificate is test-ca.pem,
    also served); returns the URL the guest uses."""
    def run(*command):
        subprocess.run(["openssl", *command], cwd=directory, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj",
        "/CN=Vexa test CA", "-keyout", "ca.key", "-out", "test-ca.pem")
    run("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=10.0.2.2",
        "-keyout", "server.key", "-out", "server.csr")
    with open(os.path.join(directory, "server.ext"), "w") as f:
        f.write("subjectAltName=IP:10.0.2.2\n")
    run("x509", "-req", "-in", "server.csr", "-CA", "test-ca.pem", "-CAkey", "ca.key",
        "-CAcreateserial", "-days", "2", "-extfile", "server.ext", "-out", "server.pem")
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(os.path.join(directory, "server.pem"),
                            os.path.join(directory, "server.key"))
    for name in ("ca.key", "server.key", "server.csr", "server.ext"):
        os.unlink(os.path.join(directory, name))
    handler = functools.partial(QuietHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return "https://10.0.2.2:%d" % server.server_address[1]


def sound_frequency(path, start=44):
    """The pitch of the loud part of QEMU's recording of what Vexa played
    (from byte `start`), how many seconds of it there are, and where the
    recording ends now."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return 0, 0, start
    if len(data) < 48 or data[:4] != b"RIFF":
        return 0, 0, start
    channels, rate = struct.unpack("<HI", data[22:28])
    end = start + (len(data) - start) // (2 * channels) * 2 * channels
    samples = struct.unpack("<%dh" % ((end - start) // 2), data[start:end])
    left = samples[::channels]
    # The pitch of each loud 50 ms piece, and the middle one of those: a
    # moment of silence (a slow machine falling behind) or the end of an
    # earlier sound recorded late doesn't change it.
    size = rate // 20
    pitches = []
    for at in range(0, len(left) - size + 1, size):
        piece = left[at:at + size]
        if sum(abs(v) for v in piece) / size < 3000:
            continue
        crossings = [i for i in range(size - 1) if (piece[i] < 0) != (piece[i + 1] < 0)]
        if len(crossings) > 2:  # Half periods between the first crossing and the last.
            pitches.append((len(crossings) - 1) / 2 / ((crossings[-1] - crossings[0]) / rate))
    if len(pitches) < 2:
        return 0, 0, end
    pitches.sort()
    return pitches[len(pitches) // 2], len(pitches) / 20, end


def sound_heard_seconds(path, start=44):
    """How many seconds of QEMU's recording (from byte `start`) aren't
    silence (music, say, which has no one pitch), and where it ends now."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return 0, start
    if len(data) < 48 or data[:4] != b"RIFF":
        return 0, start
    channels, rate = struct.unpack("<HI", data[22:28])
    end = start + (len(data) - start) // (2 * channels) * 2 * channels
    left = struct.unpack("<%dh" % ((end - start) // 2), data[start:end])[::channels]
    size = rate // 20
    loud = sum(1 for at in range(0, len(left) - size + 1, size)
               if sum(abs(v) for v in left[at:at + size]) / size > 200)
    return loud / 20, end


# Installing (--install DISK): the Installer app puts Vexa on an empty disk
# (without the Linux programs, to be quick), from the desktop; then the
# install program itself would refuse a disk that's in use.
INSTALL_COMMANDS = [
    ("install --list", "vda", 10),
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@sendkey ctrl-spc", "desktop: search", 10),
    ("@type Install Vexa", "installer: 1 disk", 20),
    ("@sendkey down", None, 2),
    ("@sendkey spc", None, 2),
    ("@sendkey ret", None, 2),
    ("@sendkey ret", "installer: installing on vda (no Linux)", 10),
    ("@sendkey shift", "installer: copying the system to vda2", 120),
    ("@sendkey shift", "installer: done", 300),
    ("@sendkey alt-f4", "desktop: asked window", 10),
    ("@sendkey ctrl-alt-q", "desktop: asking before leaving", 20),
    ("@sendkey ret", "desktop: back to the console", 20),
    ("@sendkey ctrl-u", None, 2),
    ("install --yes vda", "error: vda is in use", 10),
]

# Starting from that disk (--installed DISK, no CD): the root file system is
# on it, and what's written there is still there the next time (--boot 2).
INSTALLED_COMMANDS = [
    ("cat /etc/installed", "installed on vda", 10),
    ("df", "ext2", 10),
    ("ls /cdrom", "ls: /cdrom: ", 10),
    # (@BOOT@: this boot's number. The first one's line shows the next boot
    # kept it.)
    ("echo boot @BOOT@ >> /home/note.txt", None, 10),
    ("cat /home/note.txt", "boot 1", 10),
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@sendkey ctrl-alt-q", "desktop: asking before leaving", 20),
    ("@sendkey ret", "desktop: back to the console", 20),
]


def insert_before_leaving_desktop(commands, extra):
    """Puts `extra` before the desktop section's Ctrl+Alt+Q (the X section has
    one too), if there is a desktop section."""
    desktop = next((i for i, c in enumerate(commands) if c[0] == "#desktop"), None)
    at = next((i for i, c in enumerate(commands)
               if desktop is not None and i > desktop and c[0] == "@sendkey ctrl-alt-q"), None)
    if at is not None:
        commands[at:at] = extra


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--screenshot", help="save a PNG of the screen at the end")
    parser.add_argument("--keep-log", action="store_true", help="print the serial log")
    parser.add_argument("--uefi", action="store_true", help="boot with UEFI firmware (OVMF)")
    parser.add_argument("--smp", type=int, default=1, help="number of CPUs")
    parser.add_argument("--memory", default="512M", help="RAM size, e.g. 512M or 6G")
    parser.add_argument("--cpu", help="QEMU CPU model, e.g. max (adds AVX, SMEP, SMAP)")
    parser.add_argument("--disks", help="directory with the test disk images")
    parser.add_argument("--iso", default="build/vexa.iso", help="ISO image to boot")
    parser.add_argument("--only", help="run only these sections (comma-separated: virgl, "
                        "shell, network, usb, desktop, linux, x, linux-net, disks)")
    parser.add_argument("--no-cd", action="store_true",
                        help="boot from the first disk (an installed Vexa), without the CD")
    parser.add_argument("--nic", default="virtio-net-pci",
                        help="QEMU's network card: virtio-net-pci, e1000, e1000e, rtl8139")
    parser.add_argument("--usb", nargs="?", const="xhci", choices=sorted(USB_DEVICES),
                        help="add a USB controller with a keyboard and a mouse on a hub (typing "
                             "and the pointer then go through USB) and run the USB checks")
    parser.add_argument("--accel", default="auto", choices=["auto", "kvm", "tcg"],
                        help="QEMU's accelerator: KVM when /dev/kvm can be opened (auto)")
    parser.add_argument("--no-linux", action="store_true",
                        help="skip the Linux subsystem checks (a LINUX_COMPAT=0 kernel)")
    parser.add_argument("--install", metavar="DISK",
                        help="install Vexa on this (empty) disk image, from the desktop")
    parser.add_argument("--installed", metavar="DISK",
                        help="start from this disk image, with Vexa installed on it (no CD)")
    parser.add_argument("--machine", default="q35", choices=["q35", "pc"],
                        help="QEMU's machine: q35 (SATA, AHCI) or pc (i440FX: IDE, as in "
                             "VirtualBox's default setup)")
    parser.add_argument("--disk-bus", default="virtio", choices=["virtio", "ide"],
                        help="with --install or --installed: how that disk is attached")
    parser.add_argument("--boot", type=int, default=1,
                        help="with --installed: which time it is that the disk starts")
    parser.add_argument("--virgl", action="store_true",
                        help="add QEMU's virtio GPU with 3D (virgl, on this machine's OpenGL; "
                             "under Xvfb if there's no display) and run the virgl checks")
    parser.add_argument("--safe-mode", action="store_true",
                        help="expect a safe mode boot (use with the ISO from "
                             "`make build/vexa-safe-mode-test.iso`)")
    args = parser.parse_args()

    tmp = tempfile.mkdtemp(prefix="vexa-test-")
    log_path = os.path.join(tmp, "serial.log")
    mon_path = os.path.join(tmp, "monitor.sock")
    open(log_path, "w").close()
    command = [
        "qemu-system-x86_64", "-M", args.machine, "-m", args.memory, "-smp", str(args.smp),
        *(["-boot", "c"] if args.no_cd or args.installed else ["-cdrom", args.iso, "-boot", "d"]),
        "-serial", "file:" + log_path, "-display", "none",
        "-no-reboot", "-monitor", "unix:" + mon_path + ",server,nowait",
        # A network card behind QEMU's user-mode NAT: DHCP gives 10.0.2.15,
        # and 10.0.2.2 is this machine. (With -nic instead of -device, QEMU's
        # q35 card has no MSI-X, and Vexa would poll it.)
        "-netdev", "user,id=net0", "-device", args.nic + ",netdev=net0",
        # Sound: an HD Audio controller and codec, whose output QEMU writes
        # to a WAV file (see "@sound").
        "-audiodev", "wav,id=snd0,path=" + os.path.join(tmp, "sound.wav"),
        "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
    ]
    disks = []
    www = os.path.join(tmp, "www")
    os.mkdir(www)
    url, sha1 = start_web_server(www)
    https_url = start_https_server(www)
    commands = list(TYPED_COMMANDS)
    if args.install or args.installed:
        commands = list(INSTALL_COMMANDS if args.install else INSTALLED_COMMANDS)
        command += ["-drive", f"file={args.install or args.installed},if={args.disk_bus},format=raw"]
        args.no_linux = True
    if not args.no_linux:
        at = next(i for i, c in enumerate(commands) if c[0] == "ps")
        commands[at:at] = LINUX_COMMANDS
    if args.usb:
        at = next((i for i, c in enumerate(commands) if c[0] == "#network"), len(commands))
        commands[at:at] = USB_COMMANDS
        insert_before_leaving_desktop(commands, USB_DESKTOP_COMMANDS)
    if args.disks:
        for name, qemu_args, offset in TEST_DISKS:
            copy = os.path.join(tmp, name)
            shutil.copyfile(os.path.join(args.disks, name), copy)
            command += [a.format(copy) for a in qemu_args]
            disks.append((copy, offset))
        commands[-1:-1] = DISK_COMMANDS
        insert_before_leaving_desktop(commands, SDK_APP_COMMANDS)
    xvfb = None
    if args.virgl:
        commands[-1:-1] = VIRGL_COMMANDS
        # virglrenderer needs a GL context from QEMU's display: GTK's, on an
        # X server (Xvfb when there's none), with the Bochs VGA still the screen.
        at = command.index("-display")
        command[at + 1] = "gtk,gl=on"
        command += ["-device", "virtio-gpu-gl-pci"]
        if not os.environ.get("DISPLAY"):
            display = ":%d" % (90 + os.getpid() % 100)
            xvfb = subprocess.Popen(["Xvfb", display, "-screen", "0", "1280x1024x24", "-nolisten",
                                     "tcp"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.environ["DISPLAY"] = display
            time.sleep(2)
    # Sections: ("#name",) markers; --only keeps some of them.
    only = set(args.only.split(",")) if args.only else None
    kept, section = [], "shell"
    for c in commands:
        if c[0].startswith("#"):
            section = c[0][1:]
        elif only is None or section in only:
            kept.append(c)
    commands = kept

    stick = os.path.join(tmp, "usb-stick.img")
    if args.usb:
        # A USB stick for the hot-plug checks: an ext2 file system with a file.
        files = os.path.join(tmp, "usb-stick")
        os.mkdir(files)
        with open(os.path.join(files, "hello.txt"), "w") as f:
            f.write("Hello from a USB stick!\n")
        subprocess.run(["mke2fs", "-q", "-t", "ext2", "-d", files, stick, "16M"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        disks.append((stick, 0))

    def fill(text):
        return (text.replace("@URL@", url).replace("@HTTPS@", https_url).replace("@SHA1@", sha1)
                .replace("@STICK@", stick).replace("@BOOT@", str(args.boot)) if text else text)
    commands = [(fill(c), fill(e), *rest) for c, e, *rest in commands]
    if args.uefi:
        command += ["-bios", OVMF]
    if args.usb:
        command += USB_DEVICES[args.usb] + USB_ATTACHED
    if args.cpu:
        command += ["-cpu", args.cpu]
    accel = args.accel
    if accel == "auto":
        accel = "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg"
    command += ["-accel", accel]
    print(f"qemu-smoke-test: {accel.upper()}" + (f", only {args.only}" if only else ""),
          flush=True)
    qemu = subprocess.Popen(command)
    failures = []
    log_base = 0
    try:
        monitor = Monitor(mon_path)
        for text in EXPECTED_BOOT_LEGACY if args.safe_mode else EXPECTED_BOOT_APIC:
            if not wait_for(log_path, text, BOOT_TIMEOUT):
                failures.append("boot: missing " + repr(text))
                break
        # Vexa starts the desktop (from the CD, with the Installer open);
        # leaving it gives the console, where the checks are typed.
        boot_desktop = ["desktop: started on a"]
        if not (args.installed or args.no_cd):
            boot_desktop.append('desktop: window 1 "Installer"')
        for text in boot_desktop if not failures else []:
            if not wait_for(log_path, text, BOOT_TIMEOUT):
                failures.append("boot: missing " + repr(text))
                break
        if not failures:
            for attempt in range(3):
                time.sleep(1)
                monitor.command("sendkey ctrl-alt-q")
                if wait_for(log_path, "desktop: asking before leaving", 10):
                    break
            monitor.command("sendkey ret")
            if not wait_for(log_path, "desktop: back to the console", 30):
                failures.append("boot: couldn't leave the desktop for the console")
        # (The checks below look only at what comes after this.)
        log_base = len(read_log(log_path))

        if not failures:
            typed = 0
            previous_start = log_base
            sound_heard = 44  # How much of QEMU's sound recording "@sound" has looked at.
            for command, expected, timeout, *count in commands:
                # Normally, how many times the text is in the log since the
                # desktop started at boot was left;
                # with --only (earlier checks skipped), whether it's in what
                # came since the command before this one was sent (what a
                # command causes can show up before the next one is sent);
                # twice if typing it echoes it.
                start = log_base
                if only is not None:
                    start, previous_start = previous_start, len(read_log(log_path))
                    echoed = expected and not command.startswith("@") and expected in command
                    count = [2 if echoed else 1]
                if command.startswith("@"):
                    # "@type text": typed without waiting for a prompt (into a
                    # window, say). Otherwise a QEMU monitor command, such as
                    # "@mouse_move 10 5".
                    time.sleep(0.5)
                    if command.startswith("@type "):
                        # A window may still be starting up and drop what's
                        # typed; if nothing happens, type the line again.
                        for attempt in range(TYPE_ATTEMPTS):
                            for key in keys_for(command[6:] + "\n"):
                                monitor.command("sendkey " + key)
                            if expected is None or wait_for(log_path, expected, timeout, *(count or [1]), start):
                                break
                        else:
                            failures.append(f"{command!r}: missing {expected!r}")
                        continue
                    if command == "@music":
                        # Something that isn't silence, for a few seconds.
                        time.sleep(6)
                        seconds, sound_heard = sound_heard_seconds(os.path.join(tmp, "sound.wav"),
                                                                   sound_heard)
                        print(f"qemu-smoke-test: heard {seconds:.1f} s of sound")
                        if seconds < 1:
                            failures.append(f"{command!r}: heard {seconds:.1f} s of sound")
                        continue
                    if command.startswith("@sound "):
                        # What QEMU recorded should be a tone of that pitch.
                        want = float(command.split()[1])
                        time.sleep(3)  # (QEMU records a little behind what's played.)
                        hz, seconds, sound_heard = sound_frequency(os.path.join(tmp, "sound.wav"),
                                                                   sound_heard)
                        print(f"qemu-smoke-test: heard {hz:.0f} Hz for {seconds:.1f} s")
                        if abs(hz - want) > want * 0.03:
                            failures.append(f"{command!r}: heard {hz:.0f} Hz")
                        continue
                    if command == "@double-click":
                        # Two left clicks, quicker than one command at a time.
                        for state in (1, 0, 1, 0):
                            monitor.command(f"mouse_button {state}")
                    else:
                        monitor.command(command[1:])
                    if expected is not None and not wait_for(log_path, expected, timeout, *(count or [1]), start):
                        failures.append(f"{command!r}: missing {expected!r}")
                    continue
                # Type only once the previous command is finished and the
                # prompt is back, so slow machines don't mix commands up.
                typed += 1
                if not wait_for(log_path, PROMPT, 300, typed):
                    failures.append(f"no prompt before typing {command!r}")
                    break
                line, interrupt = command, command.endswith("^C")
                if interrupt:
                    line = command[:-2]
                for key in keys_for(line + "\n"):
                    monitor.command("sendkey " + key)
                if interrupt:
                    time.sleep(1)
                    monitor.command("sendkey ctrl-c")
                if expected is not None and not wait_for(log_path, expected, timeout, *(count or [1]), start):
                    failures.append(f"typed {command!r}: missing {expected!r}")

        if args.screenshot:
            ppm = os.path.join(tmp, "screen.ppm")
            monitor.command("screendump " + ppm)
            time.sleep(1)
            ppm_to_png(ppm, args.screenshot)
        monitor.command("quit")
    finally:
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()
        if xvfb:
            xvfb.terminate()

    for image, offset in disks:
        clean, report = check_filesystem(image, offset, tmp)
        if not clean:
            failures.append(f"e2fsck found problems on {os.path.basename(image)}:\n{report}")
    log = read_log(log_path)
    if "VEXA KERNEL PANIC" in log:
        failures.append("kernel panicked")
    if "FAILED" in log:
        failures.append("a self-test reported FAILED")
    if args.keep_log or failures:
        print(log)
    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    if not failures:
        print("PASS: Vexa booted and answered at the keyboard")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
