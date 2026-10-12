#!/usr/bin/env python3
"""Runs Servo's network load test in the Vexa VM: servo-render loads a page over HTTP from this
machine, at 10.0.2.2 (QEMU's user-mode NAT, the host as the guest sees it), so no DNS is
needed. The page is tests/servo/font-test.html, and its title says that its text got a font,
so the load and the page's script both worked. The host's server log shows the request.

  run-net-test.py WORKDIR

WORKDIR is the one tools/servo/build-servo-vexa.sh built, with servo-render in it. This makes
WORKDIR/net.img (a FAT disk with servo-render), serves tests/servo on port 8765, boots
build/vexa.iso with the disk through tools/qemu-smoke-test.py, and waits for
"servo-render: title fonts ok". The VM's screen is kept in WORKDIR/net-vm.png, and the server's
log in WORKDIR/net-server.log. Needs mkfs.vfat and mtools."""
import atexit
import importlib.util
import os
import subprocess
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
work = os.path.abspath(sys.argv[1])
binary = os.path.join(work, "servo-render")
image = os.path.join(work, "net.img")
port = 8765
os.chdir(repo)

subprocess.run(["dd", "if=/dev/zero", "of=" + image, "bs=1M", "count=512"], check=True,
               stderr=subprocess.DEVNULL)
subprocess.run(["mkfs.vfat", "-F", "32", "-n", "SERVO", image], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["mcopy", "-i", image, binary, "::/servo-render"], check=True)

server_log = open(os.path.join(work, "net-server.log"), "w")
server = subprocess.Popen(
    [sys.executable, "-m", "http.server", str(port), "--bind", "0.0.0.0",
     "--directory", os.path.join(repo, "tests", "servo")],
    stdout=server_log, stderr=subprocess.STDOUT)
atexit.register(server.terminate)

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
    ("ls -l /mnt/vda", "servo-render", 20),
    # A control: Vexa's own fetch of the same page, from the same server.
    (f"fetch -o /mnt/vda/control.html http://10.0.2.2:{port}/font-test.html", None, 60),
    # Servo's network log, on the console, for the load below.
    ("export RUST_LOG=net=debug,warn", None, 5),
    (f"/mnt/vda/servo-render http://10.0.2.2:{port}/font-test.html /mnt/vda/net.png",
     "servo-render: title fonts ok", 900),
    # The title comes before the frame is saved; this waits until the file is there.
    ("ls -l /mnt/vda", "net.png", 900),
]
sys.argv = ["qemu-smoke-test.py", "--no-linux", "--only", "shell", "--keep-log",
            "--memory", "2G", "--screenshot", os.path.join(work, "net-vm.png")]
sys.exit(smoke.main())
