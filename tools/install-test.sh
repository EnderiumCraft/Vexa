#!/bin/sh
# Installs Vexa on an empty disk image with the Installer app (from the CD),
# checks the new file system, and starts from the disk, without the CD, with
# BIOS and then UEFI firmware (the second time, what the first wrote is there,
# from an IDE disk).
set -e
dir=$(mktemp -d /tmp/vexa-install-XXXXXX)
trap 'rm -rf "$dir"' EXIT
disk=$dir/disk.img
truncate -s 768M "$disk"
tools/qemu-smoke-test.py --memory 1G --install "$disk"
# The root partition: the second, after the 1 MiB gap and the 128 MiB ESP.
if command -v e2fsck > /dev/null; then
    dd if="$disk" of="$dir/root.img" bs=1M skip=129 status=none
    e2fsck -fn "$dir/root.img"
    rm -f "$dir/root.img"
fi
tools/qemu-smoke-test.py --memory 1G --installed "$disk"
# (The second time on an older PC's IDE: QEMU's i440FX machine.)
tools/qemu-smoke-test.py --memory 1G --installed "$disk" --uefi --boot 2 --machine pc --disk-bus ide
