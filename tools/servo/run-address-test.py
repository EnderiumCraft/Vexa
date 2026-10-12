#!/usr/bin/env python3
"""Runs the address bar test in the Vexa VM: Vane opens a window on the desktop with
tests/servo/window-test.html in it, and the test clicks the address bar, types the address of
tests/servo/font-test.html and presses Enter. The page loads, and its title, "fonts ok", shows
that the address was used.

  run-address-test.py WORKDIR

WORKDIR is the one tools/servo/build-servo-vexa.sh built, with vane in it. This makes
WORKDIR/address.img (a FAT disk with vane and the two pages), boots build/vexa.iso with it
through tools/qemu-smoke-test.py, and waits for each step. The VM's screen is kept in
WORKDIR/address-vm.png, so the bar can be looked at. Needs mkfs.vfat and mtools."""
import importlib.util
import os
import subprocess
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
work = os.path.abspath(sys.argv[1])
binary = os.path.join(work, "vane")
image = os.path.join(work, "address.img")
os.chdir(repo)

subprocess.run(["dd", "if=/dev/zero", "of=" + image, "bs=1M", "count=512"], check=True,
               stderr=subprocess.DEVNULL)
subprocess.run(["mkfs.vfat", "-F", "32", "-n", "ADDRESS", image], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["mcopy", "-i", image, binary, "::/vane"], check=True)
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
# The window starts on font-test.html, which is still: the window test's page animates, and a
# window that keeps painting reads its keys too slowly. The address typed is window-test.html,
# which is what the window test runs, so its title shows the navigation. Keys are typed one at a
# time, each after a half-second wait (every "@" command waits that long first).
address = "file:///mnt/vda/window-test.html"
typed = [("@sendkey " + key, None, 1) for key in smoke.keys_for(address)]
smoke.TYPED_COMMANDS = [
    ("#shell",),
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@type /mnt/vda/vane file:///mnt/vda/font-test.html > /dev/console",
     "vane: page loaded", 900),
    # The 960x680 window is at (160, 89) on the screen. The address field is in its toolbar, at
    # (120..794, 41..67) in the window: its middle is at (677, 143) on the screen. A press there
    # takes the keyboard for the field.
    ("@mouse_move -640 -400", None, 5),
    ("@mouse_move 677 143", None, 5),
    ("@mouse_button 1", "vane: address focused", 60),
    ("@mouse_button 0", None, 3),
] + typed + [
    # Enter loads what the bar holds: the first typed character replaced the address.
    ("@sendkey ret", "vane: navigate " + address, 60),
    # The page loads and sets its title. A mouse move with no movement only gives the step a
    # reason to wait on the console; the title is printed as the page's frames come in.
    ("@mouse_move 0 0", "vane: title window test", 120),
    # The page's animation frames come after its title: wait for the title they set. Painting
    # is slow, and the frame with the page's blue corner comes later still: the screenshot is
    # taken once the program has shown it.
    ("@mouse_move 0 0", "vane: title scroll", 120),
    ("@mouse_move 0 0", "vane: corner #0000ff", 400),
    # The program shows the frame right after saying so, and the desktop draws it a moment later.
] + [("@mouse_move 0 0", None, 1)] * 6 + [
]
sys.argv = ["qemu-smoke-test.py", "--no-linux", "--only", "shell", "--keep-log",
            "--memory", "2G", "--screenshot", os.path.join(work, "address-vm.png")]
sys.exit(smoke.main())
