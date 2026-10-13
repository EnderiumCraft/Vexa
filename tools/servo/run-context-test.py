#!/usr/bin/env python3
"""Runs Vane's context menu test in the Vexa VM: Vane opens tests/servo/link-test.html, a page
with a link and an icon. A right click on the link opens the page's context menu (Vane prints its
items), the keyboard moves down it to "Open Link in New Tab" and Enter chooses it: the link's page,
font-test.html, opens in a second tab.

  run-context-test.py WORKDIR

WORKDIR is the one tools/servo/build-servo-vexa.sh built, with vane in it. This makes
WORKDIR/context.img (a FAT disk with vane and the pages), boots build/vexa.iso with it through
tools/qemu-smoke-test.py, and waits for each step. The VM's screen is kept in
WORKDIR/context-vm.png. Needs mkfs.vfat and mtools."""
import importlib.util
import os
import subprocess
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
work = os.path.abspath(sys.argv[1])
binary = os.path.join(work, "vane")
image = os.path.join(work, "context.img")
os.chdir(repo)

subprocess.run(["dd", "if=/dev/zero", "of=" + image, "bs=1M", "count=512"], check=True,
               stderr=subprocess.DEVNULL)
subprocess.run(["mkfs.vfat", "-F", "32", "-n", "CONTEXT", image], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["mcopy", "-i", image, binary, "::/vane"], check=True)
for page in ("link-test.html", "font-test.html"):
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
# The 960x680 window is at (160, 89) on the screen, and its page starts 74 pixels down: the link,
# at (100..320, 100..130) in the page, has (280, 278) on the screen.
smoke.TYPED_COMMANDS = [
    ("#shell",),
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@type /mnt/vda/vane file:///mnt/vda/link-test.html > /dev/console",
     "vane: title link test", 900),
    ("@mouse_move -640 -400", None, 5),
    ("@mouse_move 280 278", None, 5),
    # A right click (the monitor's button 2): the page asks for its context menu.
    ("@mouse_button 2", "vane: context menu", 300),
    ("@mouse_button 0", None, 3),
    # Down to the first item, which opens the link in a new tab (Vane prints the items in the log).
    ("@sendkey down", None, 2),
    ("@sendkey ret", "vane: tab 2 of 2", 300),
    ("@mouse_move 0 0", "vane: title fonts ok", 300),
] + [("@mouse_move 0 0", None, 1)] * 6
sys.argv = ["qemu-smoke-test.py", "--no-linux", "--only", "shell", "--keep-log",
            "--memory", "2G", "--screenshot", os.path.join(work, "context-vm.png")]
sys.exit(smoke.main())
