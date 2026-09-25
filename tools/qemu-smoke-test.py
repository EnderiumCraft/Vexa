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
# is typed without waiting for vsh's prompt first.
TYPED_COMMANDS = [
    ("help", "run in the background", 10),
    ("hello", "hi :)", 10),
    ("uptime", "MiB memory free", 10),
    ("ls /", "README.txt", 10),
    ("ls /bin", "vsh", 10, 2),
    ("cat /etc/motd", "Welcome to Vexa", 10, 2),
    ("ln -s /etc/motd /tmp/motd-link ; cat /tmp/motd-link", "Welcome to Vexa", 10, 3),
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
    # The kernel monitor's commands, through `sys`.
    # Threads: four sharing a mutex, then exiting with threads still running.
    ("thread-test", "thread-test: passed", 60),
    ("thread-test exit", "exiting with threads still running", 30),
    ("sys mem", "heap ", 10),
    ("sys threads", "idle", 10),
    ("sys memtest", "memtest: passed", 180),
    # Networking: DHCP, sockets over loopback, and a download from this machine
    # (@URL@ is the test's HTTP server).
    ("net", "address 10.0.2.15", 10),
    ("socket-test", "socket-test: passed", 60),
    ("fetch @URL@/hello.txt", "Hello from the test's web server", 30),
    # Graphics: the mouse, then the desktop with a terminal window. Typing
    # goes to the window's shell; dragging its title bar moves it (the pointer
    # starts in the middle of the 1280x800 screen; QEMU can drop part of a
    # long move, so those are split); Ctrl+Alt+T opens another; Ctrl+Alt+Q
    # goes back.
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
    ("@mouse_move 0 77", None, 5),
    ("@mouse_button 1", 'desktop: window 3 "About Vexa"', 20),
    ("@mouse_button 0", None, 5),
    ("@mouse_move 463 37", None, 5),
    ("@mouse_button 1", "desktop: asked window 3 to close", 10),
    ("@mouse_button 0", "desktop: closed window 3", 10),
    ("@sendkey ctrl-alt-q", "desktop: back to the console", 20),
    ("Hello Vexa", "Hello: command not found", 10),
]

# The Linux subsystem: BusyBox (built from source with musl) from vsh, then
# its shell, with fork, exec, pipes, Ctrl-C and signal handlers. Typed before
# "ps" unless --no-linux (for kernels built with LINUX_COMPAT=0).
TYPE_ATTEMPTS = 3  # For "@type" lines (see main).

LINUX_COMMANDS = [
    ("busybox echo hello from linux", "hello from linux", 20, 2),
    ("busybox uname -sr", "Vexa 6.1.0-vexa", 20),
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
    # X: the desktop, then Xvexa (an X server in a desktop window) with an
    # xterm (Ctrl+Alt+X), whose shell gets what's typed into it once the
    # pointer is over it (X without a window manager: focus follows the mouse).
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@sendkey ctrl-alt-x", 'desktop: window 2 "X (:0)"', 60),
    ("@mouse_move -440 -200", "xterm: cannot load font", 60),
    ("@type echo typed-in-xterm > /dev/console", "typed-in-xterm", 20),
    ("@sendkey ctrl-alt-q", "desktop: back to the console", 30),
    # Networking: BSD sockets (with SCM_RIGHTS), wget, ifconfig, ping and
    # Python's urllib, asyncio and multiprocessing pipes.
    ("bsd-socket-test", "bsd-socket-test: passed", 60),
    ("wget -q -O /tmp/data.bin @URL@/data.bin", None, 60),
    ("sha1sum /tmp/data.bin", "@SHA1@", 30),
    ("ifconfig eth0", "inet addr:10.0.2.15", 20),
    ("ping -c 2 127.0.0.1", "2 packets received", 30),
    ("python-net-test.py @URL@/data.bin @SHA1@", "python-net-test: passed", 300),
]

# With --disks: one ext2 file system on each kind of disk.
DISK_COMMANDS = [
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
]

# (file name in the disks directory, QEMU arguments, where the ext2 starts)
TEST_DISKS = [
    ("virtio-gpt.img", ["-drive", "file={},if=virtio,format=raw"], 1024 * 1024),
    ("sata-mbr.img", ["-drive", "file={},if=none,id=sata0,format=raw",
                      "-device", "ide-hd,drive=sata0,bus=ide.0"], 1024 * 1024),
    ("nvme-whole.img", ["-drive", "file={},if=none,id=nvme0,format=raw",
                        "-device", "nvme,serial=vexa0,drive=nvme0"], 0),
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


def wait_for(log_path, text, timeout, count=1):
    deadline = time.time() + timeout
    while time.time() < deadline:
        log = read_log(log_path)
        if "VEXA KERNEL PANIC" in log:
            return False
        if log.count(text) >= count:
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


def start_web_server(directory):
    """Serves `directory` on a free port of this machine; returns the URL the
    guest uses for it, and the SHA-1 of data.bin."""
    with open(os.path.join(directory, "hello.txt"), "w") as f:
        f.write("Hello from the test's web server\n")
    # A megabyte that isn't all the same byte, so corruption would show.
    data = b"".join(hashlib.sha256(str(i).encode()).digest() for i in range(32768))
    with open(os.path.join(directory, "data.bin"), "wb") as f:
        f.write(data)
    handler = functools.partial(QuietHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    # QEMU's user-mode network shows this machine to the guest as 10.0.2.2.
    return "http://10.0.2.2:%d" % server.server_address[1], hashlib.sha1(data).hexdigest()


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
    parser.add_argument("--no-linux", action="store_true",
                        help="skip the Linux subsystem checks (a LINUX_COMPAT=0 kernel)")
    parser.add_argument("--safe-mode", action="store_true",
                        help="expect a safe mode boot (use with the ISO from "
                             "`make build/vexa-safe-mode-test.iso`)")
    args = parser.parse_args()

    tmp = tempfile.mkdtemp(prefix="vexa-test-")
    log_path = os.path.join(tmp, "serial.log")
    mon_path = os.path.join(tmp, "monitor.sock")
    open(log_path, "w").close()
    command = [
        "qemu-system-x86_64", "-M", "q35", "-m", args.memory, "-smp", str(args.smp),
        "-cdrom", args.iso, "-boot", "d", "-serial", "file:" + log_path, "-display", "none",
        "-no-reboot", "-monitor", "unix:" + mon_path + ",server,nowait",
        # A network card behind QEMU's user-mode NAT: DHCP gives 10.0.2.15,
        # and 10.0.2.2 is this machine. (With -nic instead of -device, QEMU's
        # q35 card has no MSI-X, and Vexa would poll it.)
        "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0",
    ]
    disks = []
    www = os.path.join(tmp, "www")
    os.mkdir(www)
    url, sha1 = start_web_server(www)
    commands = list(TYPED_COMMANDS)
    if not args.no_linux:
        at = next(i for i, c in enumerate(commands) if c[0] == "ps")
        commands[at:at] = LINUX_COMMANDS
    if args.disks:
        for name, qemu_args, offset in TEST_DISKS:
            copy = os.path.join(tmp, name)
            shutil.copyfile(os.path.join(args.disks, name), copy)
            command += [a.format(copy) for a in qemu_args]
            disks.append((copy, offset))
        commands[-1:-1] = DISK_COMMANDS
    def fill(text):
        return text.replace("@URL@", url).replace("@SHA1@", sha1) if text else text
    commands = [(fill(c), fill(e), *rest) for c, e, *rest in commands]
    if args.uefi:
        command += ["-bios", OVMF]
    if args.cpu:
        command += ["-cpu", args.cpu]
    qemu = subprocess.Popen(command)
    failures = []
    try:
        monitor = Monitor(mon_path)
        for text in EXPECTED_BOOT_LEGACY if args.safe_mode else EXPECTED_BOOT_APIC:
            if not wait_for(log_path, text, BOOT_TIMEOUT):
                failures.append("boot: missing " + repr(text))
                break

        if not failures:
            typed = 0
            for command, expected, timeout, *count in commands:
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
                            if expected is None or wait_for(log_path, expected, timeout, *count):
                                break
                        else:
                            failures.append(f"{command!r}: missing {expected!r}")
                        continue
                    monitor.command(command[1:])
                    if expected is not None and not wait_for(log_path, expected, timeout, *count):
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
                if expected is not None and not wait_for(log_path, expected, timeout, *count):
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
