#!/usr/bin/env python3
"""Boot build/vexa.iso in QEMU, type into the PS/2 keyboard, and check the serial log.

Usage: tools/qemu-smoke-test.py [--uefi] [--smp N] [--safe-mode] [--screenshot out.png]
                                [--keep-log]

Exits non-zero if an expected message is missing or the kernel panics.
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ISO = "build/vexa.iso"
BOOT_TIMEOUT = 30
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

# Typed at the monitor prompt, with text that must appear in response.
# The repeated "help" fills the screen so the console has to scroll.
TYPED_COMMANDS = [
    ("help", "reboot"),
    ("help", "reboot"),
    ("help", "reboot"),
    ("cpux\b", "vendor"),  # Backspace erases the typo, so this runs "cpu".
    ("uptime", "up "),
    ("Hello Vexa", "unknown command: Hello Vexa"),
]

# QEMU `sendkey` names for characters that aren't plain lowercase letters or digits.
KEY_NAMES = {" ": "spc", "\n": "ret", "\b": "backspace", "-": "minus", ".": "dot", "/": "slash"}


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


def wait_for(log_path, text, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        log = read_log(log_path)
        if "VEXA KERNEL PANIC" in log:
            return False
        if text in log:
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
    parser.add_argument("--safe-mode", action="store_true",
                        help="pick the safe mode boot entry (tests the PIC/PIT fallback)")
    args = parser.parse_args()

    tmp = tempfile.mkdtemp(prefix="vexa-test-")
    log_path = os.path.join(tmp, "serial.log")
    mon_path = os.path.join(tmp, "monitor.sock")
    open(log_path, "w").close()
    command = [
        "qemu-system-x86_64", "-M", "q35", "-m", "512M", "-smp", str(args.smp),
        "-cdrom", ISO, "-serial", "file:" + log_path, "-display", "none", "-no-reboot",
        "-monitor", "unix:" + mon_path + ",server,nowait",
    ]
    if args.uefi:
        command += ["-bios", OVMF]
    qemu = subprocess.Popen(command)
    failures = []
    try:
        monitor = Monitor(mon_path)
        if args.safe_mode:
            # Choose the second entry in the bootloader menu before its timeout.
            time.sleep(1.5)
            monitor.command("sendkey down")
            monitor.command("sendkey ret")
        for text in EXPECTED_BOOT_LEGACY if args.safe_mode else EXPECTED_BOOT_APIC:
            if not wait_for(log_path, text, BOOT_TIMEOUT):
                failures.append("boot: missing " + repr(text))
                break

        if not failures:
            for command, expected in TYPED_COMMANDS:
                for key in keys_for(command + "\n"):
                    monitor.command("sendkey " + key)
                if not wait_for(log_path, expected, 10):
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

    log = read_log(log_path)
    if "VEXA KERNEL PANIC" in log:
        failures.append("kernel panicked")
    if args.keep_log or failures:
        print(log)
    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    if not failures:
        print("PASS: Vexa booted and answered at the keyboard")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
