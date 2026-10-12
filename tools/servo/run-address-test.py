#!/usr/bin/env python3
"""Runs the address bar test in the Vexa VM: servo-window opens a window on the desktop with
tests/servo/window-test.html in it, and the test clicks the address bar, types the address of
tests/servo/font-test.html and presses Enter. The page loads, and its title, "fonts ok", shows
that the address was used.

  run-address-test.py WORKDIR

WORKDIR is the one tools/servo/build-servo-vexa.sh built, with servo-window in it. This makes
WORKDIR/address.img (a FAT disk with servo-window and the two pages), boots build/vexa.iso with it
through tools/qemu-smoke-test.py, and waits for each step. The VM's screen is kept in
WORKDIR/address-vm.png, so the bar can be looked at. Needs mkfs.vfat and mtools."""
import importlib.util
import os
import subprocess
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
work = os.path.abspath(sys.argv[1])
binary = os.path.join(work, "servo-window")
image = os.path.join(work, "address.img")
os.chdir(repo)

subprocess.run(["dd", "if=/dev/zero", "of=" + image, "bs=1M", "count=512"], check=True,
               stderr=subprocess.DEVNULL)
subprocess.run(["mkfs.vfat", "-F", "32", "-n", "ADDRESS", image], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["mcopy", "-i", image, binary, "::/servo-window"], check=True)
for page in ("window-test.html", "font-test.html"):
    subprocess.run(["mcopy", "-i", image, os.path.join(repo, "tests", "servo", page),
                    "::/" + page], check=True)

spec = importlib.util.spec_from_file_location(
    "smoke", os.path.join(repo, "tools", "qemu-smoke-test.py"))
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)
qemu_popen = smoke.subprocess.Popen


def popen_with_disk(cmd, *args, **kwargs):
    if isinstance(cmd, list) and cmd and cmd[0].startswith("qemu"):
        cmd = cmd + ["-drive", "file=%s,if=virtio,format=raw" % image]
    return qemu_popen(cmd, *args, **kwargs)


smoke.subprocess.Popen = popen_with_disk
smoke.TYPED_COMMANDS = [
    ("#shell",),
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@type /mnt/vda/servo-window file:///mnt/vda/window-test.html > /dev/console",
     "servo-window: page loaded", 900),
    # The window's content is at (240, 129) on the screen, and the address bar is its top 40
    # pixels: the field's middle is at (640, 149). A press there takes the keyboard for the bar.
    ("@mouse_move -640 -400", None, 5),
    ("@mouse_move 640 149", None, 5),
    ("@mouse_button 1", "servo-window: address focused", 60),
    ("@mouse_button 0", None, 3),
    # The first typed character replaces the address, and Enter loads what the bar holds.
    ("@type file:///mnt/vda/font-test.html", None, 10),
    ("@sendkey ret", "servo-window: navigate file:///mnt/vda/font-test.html", 60),
    # The page loads and sets its title. A mouse move with no movement only gives the step a
    # reason to wait on the console; the title is printed as the page's frames come in.
    ("@mouse_move 0 0", "servo-window: title fonts ok", 120),
]
sys.argv = ["qemu-smoke-test.py", "--no-linux", "--only", "shell", "--keep-log",
            "--memory", "2G", "--screenshot", os.path.join(work, "address-vm.png")]
sys.exit(smoke.main())
