#!/usr/bin/env python3
"""Runs the tabs test in the Vexa VM: Vane opens font-test.html, and the test opens a second tab
with Ctrl+T, loads window-test.html in it, moves between the tabs with Ctrl+1 and Ctrl+2, zooms,
closes the tab with Ctrl+W, and goes back and forward through the first tab's history with
Alt+Left and Alt+Right.

  run-tabs-test.py WORKDIR

WORKDIR is the one tools/servo/build-servo-vexa.sh built, with vane in it. This makes
WORKDIR/tabs.img (a FAT disk with vane and the two pages), boots build/vexa.iso with it
through tools/qemu-smoke-test.py, and waits for each step. The VM's screen is kept in
WORKDIR/tabs-vm.png, so the tabs can be looked at. Needs mkfs.vfat and mtools."""
import importlib.util
import os
import subprocess
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
work = os.path.abspath(sys.argv[1])
binary = os.path.join(work, "vane")
image = os.path.join(work, "tabs.img")
os.chdir(repo)

subprocess.run(["dd", "if=/dev/zero", "of=" + image, "bs=1M", "count=512"], check=True,
               stderr=subprocess.DEVNULL)
subprocess.run(["mkfs.vfat", "-F", "32", "-n", "TABS", image], check=True,
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
# Keys are typed one at a time, each after a half-second wait (every "@" command waits that long
# first): the window keeps painting, and a burst of keys overruns the VM's keyboard buffer.
second = "file:///mnt/vda/window-test.html"
typed = [("@sendkey " + key, None, 1) for key in smoke.keys_for(second)]
smoke.TYPED_COMMANDS = [
    ("#shell",),
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@type /mnt/vda/vane file:///mnt/vda/font-test.html > /dev/console",
     "vane: page loaded", 900),
    # Ctrl+T opens a tab, which has the address bar to type in.
    ("@sendkey ctrl-t", "vane: tab 2 of 2", 60),
] + typed + [
    ("@sendkey ret", "vane: navigate " + second, 60),
    ("@mouse_move 0 0", "vane: title window test", 300),
    # Ctrl+1 and Ctrl+2 pick a tab by its place.
    ("@sendkey ctrl-1", "vane: tab 1 of 2", 60),
    ("@sendkey ctrl-2", "vane: tab 2 of 2", 60),
    # Ctrl+Plus zooms in a step (100% to 110%), and Ctrl+0 is back to 100%.
    ("@sendkey ctrl-equal", "vane: zoom 110", 60),
    ("@sendkey ctrl-0", "vane: zoom 100", 60),
    # Ctrl+W closes the tab, which leaves the first.
    ("@sendkey ctrl-w", "vane: tab 1 of 1", 60),
    # In the first tab: load window-test.html over font-test.html, go back, and go forward.
    ("@sendkey ctrl-l", "vane: address focused", 60),
] + typed + [
    ("@sendkey ret", "vane: navigate " + second, 60),
    ("@mouse_move 0 0", "vane: title window test", 300),
    ("@sendkey alt-left", "vane: title fonts ok", 300),
    # Going forward shows window-test.html again, with its blue corner. (Its title stays what
    # the page's key handler made of the Alt key, so the corner is what the test waits for.)
    ("@sendkey alt-right", "vane: corner #0000ff", 400),
] + [("@mouse_move 0 0", None, 1)] * 6
sys.argv = ["qemu-smoke-test.py", "--no-linux", "--only", "shell", "--keep-log",
            "--memory", "2G", "--screenshot", os.path.join(work, "tabs-vm.png")]
sys.exit(smoke.main())
