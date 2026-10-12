#!/usr/bin/env python3
"""Runs Servo's render test in the Vexa VM: servo-render loads tests/servo/render-test.html
from a FAT disk, paints it, and checks the pixels.

  run-render-test.py WORKDIR

WORKDIR is the one tools/servo/build-servo-vexa.sh built, with servo-render in it. This makes
WORKDIR/render.img (a FAT disk with servo-render and the page), boots build/vexa.iso with it
through tools/qemu-smoke-test.py, and waits for "servo-render: passed". The render is copied to
WORKDIR/render-out.png, and the VM's screen to WORKDIR/render-vm.png. Needs mkfs.vfat and mtools."""
import atexit
import importlib.util
import os
import subprocess
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
work = os.path.abspath(sys.argv[1])
binary = os.path.join(work, "servo-render")
page = os.path.join(repo, "tests", "servo", "render-test.html")
image = os.path.join(work, "render.img")
os.chdir(repo)

subprocess.run(["dd", "if=/dev/zero", "of=" + image, "bs=1M", "count=512"], check=True,
               stderr=subprocess.DEVNULL)
subprocess.run(["mkfs.vfat", "-F", "32", "-n", "SERVO", image], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["mcopy", "-i", image, binary, "::/servo-render"], check=True)
subprocess.run(["mcopy", "-i", image, page, "::/render-test.html"], check=True)
# The VM writes its render to the disk; copy it out once the VM has stopped.
atexit.register(subprocess.run, ["mcopy", "-o", "-i", image, "::/render.png",
                                 os.path.join(work, "render-out.png")],
                stderr=subprocess.DEVNULL)

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
    ("ls -l /mnt/vda", "render-test.html", 20),
    ("/mnt/vda/servo-render file:///mnt/vda/render-test.html /mnt/vda/render.png",
     "servo-render: passed", 900),
]
sys.argv = ["qemu-smoke-test.py", "--no-linux", "--only", "shell", "--keep-log",
            "--memory", "2G", "--screenshot", os.path.join(work, "render-vm.png")]
smoke.main()
