#!/usr/bin/env python3
"""Runs Vane as the ISO has it, in the Vexa VM: from /bin/vane (a link to the boot CD's
/cdrom/vane/vane), with no address, so it opens the start page of the Vane app bundle
(/apps/Vane.vxapp/Contents/Resources/start.html). Its title, "New Tab", shows that the program
and the page were found. The ISO must have been built with Vane (`make vane iso`).

  run-vane-iso-test.py [SCREENSHOT.png]

The VM's screen is kept in the screenshot file (default build/vane-iso-vm.png)."""
import importlib.util
import os
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
screenshot = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(repo, "build", "vane-iso-vm.png")
os.chdir(repo)

spec = importlib.util.spec_from_file_location(
    "smoke", os.path.join(repo, "tools", "qemu-smoke-test.py"))
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)

smoke.TYPED_COMMANDS = [
    ("#shell",),
    ("desktop", 'desktop: window 1 "Terminal"', 30),
    ("@type vane > /dev/console", "vane: title New Tab", 600),
    # The start page's frame comes some seconds later: the program says when its top left
    # corner is painted (the page's background is not white), so the screenshot shows it.
    ("@mouse_move 0 0", "vane: corner #", 300),
] + [("@mouse_move 0 0", None, 1)] * 6
sys.argv = ["qemu-smoke-test.py", "--no-linux", "--only", "shell", "--keep-log",
            "--memory", "2G", "--screenshot", screenshot]
sys.exit(smoke.main())
