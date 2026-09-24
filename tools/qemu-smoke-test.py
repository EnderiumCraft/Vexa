#!/usr/bin/env python3
"""Boot build/vexa.iso in QEMU, type into the PS/2 keyboard, and check the serial log.

Usage: tools/qemu-smoke-test.py [--iso PATH] [--uefi] [--smp N] [--memory SIZE] [--cpu MODEL]
                                [--safe-mode] [--disks DIR]

With --disks, attaches the test disks from `make test-disks` (copies, so the
originals stay pristine), runs file commands on them, and checks each with
e2fsck afterwards.
                                [--screenshot out.png] [--keep-log]

Exits non-zero if an expected message is missing or the kernel panics.
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

BOOT_TIMEOUT = 60
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

# Typed at the monitor prompt, with text that must appear in response, how
# many seconds to wait for it, and optionally how many times it must appear in
# the whole log. The repeated "help" makes the console scroll.
TYPED_COMMANDS = [
    ("help", "reboot", 10),
    ("help", "reboot", 10),
    ("help", "reboot", 10),
    ("hello", "hi :)\r\nVexa 0.", 10),
    ("cpux\b", "vendor", 10),  # Backspace erases the typo, so this runs "cpu".
    ("uptime", "up ", 10),
    ("mem", "heap ", 10),
    ("memtest", "memtest: passed", 180),
    ("programs", "hello-world", 10),
    # Files: the root file system unpacked from the initramfs, and /dev.
    ("ls /", "README.txt", 10),
    ("mount", "devfs", 10),
    ("write /tmp/note.txt hello from the monitor", "vexa> write", 10),
    ("cat /tmp/note.txt", "hello from the monitor", 10, 2),
    ("run fs-test", "fs-test: passed", 60),
    # The Phase 3 milestone: a native program in user mode.
    ("run hello-world", "running in user mode.", 30),
    ("run hello-world", "exited with code 0", 30, 2),
    # A program that misbehaves is stopped, and the system carries on.
    ("run crash", "kernel pointer rejected", 30),
    ("hello", "killed: Page Fault", 30),
    # Three programs at once, each checking its vector registers survive.
    ("spawn fpu-stress", "started fpu-stress", 10),
    ("spawn fpu-stress", "started fpu-stress", 10, 2),
    ("run fpu-stress", "): passed, 20 rounds", 300, 3),
    ("threads", "monitor", 10),
    ("Hello Vexa", "unknown command: Hello", 10),
]

# With --disks: one ext2 file system on each kind of disk.
DISK_COMMANDS = [
    ("disks", "nvme0n1", 10),
    ("mount", "/mnt/nvme0n1", 10),
    ("cat /mnt/vda1/hello.txt", "Hello from an ext2 disk!", 10),
    ("cat /mnt/sda1/docs/notes.txt", "lives on a test disk", 10),
    ("run /mnt/nvme0n1/hello-world", "Hello, world!", 30, 3),
    ("run fs-test", "on the disk at /mnt/vda1", 120),
    ("write /mnt/sda1/from-vexa.txt written on sata", "vexa> write /mnt/sda1", 10),
    ("cat /mnt/sda1/from-vexa.txt", "written on sata", 10, 2),
    ("mkdir /mnt/nvme0n1/made-by-vexa", "vexa> mkdir", 10),
    ("write /mnt/nvme0n1/made-by-vexa/note.txt written on nvme", "vexa> write /mnt/nvme0n1", 10),
    ("cat /mnt/nvme0n1/made-by-vexa/note.txt", "written on nvme", 10, 2),
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
             ":": "shift-semicolon", "_": "shift-minus"}


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
    ]
    disks = []
    commands = list(TYPED_COMMANDS)
    if args.disks:
        for name, qemu_args, offset in TEST_DISKS:
            copy = os.path.join(tmp, name)
            shutil.copyfile(os.path.join(args.disks, name), copy)
            command += [a.format(copy) for a in qemu_args]
            disks.append((copy, offset))
        commands[-1:-1] = DISK_COMMANDS
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
            for command, expected, timeout, *count in commands:
                for key in keys_for(command + "\n"):
                    monitor.command("sendkey " + key)
                if not wait_for(log_path, expected, timeout, *count):
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
