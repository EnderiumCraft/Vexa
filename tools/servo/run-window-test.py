#!/usr/bin/env python3
"""Runs the window test in the Vexa VM: servo-window opens a window on the desktop with
tests/servo/window-test.html in it, and the test drives the pointer, the wheel, the keyboard
and a resize through the desktop. The page sets its title on each event, and servo-window
prints each title, so the serial log shows the events arriving.

  run-window-test.py WORKDIR

WORKDIR is the one tools/servo/build-servo-vexa.sh built, with servo-window in it. This makes
WORKDIR/window.img (a FAT disk with servo-window and the page), boots build/vexa.iso with it
through tools/qemu-smoke-test.py, and waits for each step. The VM's screen is kept in
WORKDIR/window-vm.png. Needs mkfs.vfat and mtools."""
import importlib.util
import os
import subprocess
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
work = os.path.abspath(sys.argv[1])
binary = os.path.join(work, "servo-window")
page = os.path.join(repo, "tests", "servo", "window-test.html")
image = os.path.join(work, "window.img")
os.chdir(repo)

subprocess.run(["dd", "if=/dev/zero", "of=" + image, "bs=1M", "count=512"], check=True,
               stderr=subprocess.DEVNULL)
subprocess.run(["mkfs.vfat", "-F", "32", "-n", "SERVO", image], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["mcopy", "-i", image, binary, "::/servo-window"], check=True)
subprocess.run(["mcopy", "-i", image, page, "::/window-test.html"], check=True)

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
    # The pointer starts at the middle of the 1280x800 screen. The window's content is at
    # (240, 129) on the screen, and the field is at (250, 215) in the page.
    ("@mouse_move -640 -400", None, 5),
    ("@mouse_move 490 344", None, 5),
    ("@mouse_button 1", "servo-window: title mousedown 250,215", 60),
    ("@mouse_button 0", None, 3),
    # The keyboard: the field has the focus after the press, so the typed text reaches it.
    ("@type hi", "servo-window: title typed hi", 60),
    # The wheel: a notch scrolls the page, and the page reports its new offset.
    ("@mouse_move 0 0 -1", "servo-window: title wheel ", 60),
    ("@mouse_move 0 0 0", "servo-window: title scroll 40 ", 60),
    # The window: snapping it to the top left resizes it, and the page reports the new size.
    ("@sendkey meta_l-up", "size 1278x744", 60),
]
sys.argv = ["qemu-smoke-test.py", "--no-linux", "--only", "shell", "--keep-log",
            "--memory", "2G", "--screenshot", os.path.join(work, "window-vm.png")]
sys.exit(smoke.main())
