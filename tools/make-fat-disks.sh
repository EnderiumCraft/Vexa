#!/bin/sh
# Builds the FAT32 and exFAT test disks, the way other systems make them
# (mkfs.vfat and mkfs.exfat), for Vexa's FAT and exFAT support. The FAT32
# one gets files with long names (spaces, accents), a directory bigger than
# a cluster and a 1 MiB file; the exFAT one starts empty (the smoke test
# writes to it, and fsck.exfat checks it afterwards).
# Usage: tools/make-fat-disks.sh FAT32-OUTPUT EXFAT-OUTPUT
set -eu
fat=$1 exfat=$2
content=$(mktemp -d)
trap 'rm -rf "$content"' EXIT
export LC_ALL=C.UTF-8 MTOOLS_SKIP_CHECK=1
echo "Hello from a FAT32 disk!" > "$content/hello.txt"
echo "A long name, with spaces" > "$content/A long file name with spaces.txt"
mkdir -p "$content/Documents" "$content/many"
echo "Accents in a long name" > "$content/Documents/Ünïcödé name.txt"
i=1
while [ $i -le 300 ]; do
    echo $i > "$content/many/file-$i.txt"
    i=$((i + 1))
done
python3 -c "
import sys
sys.stdout.buffer.write(bytes((i * 7 + i // 251) & 255 for i in range(1 << 20)))
" > "$content/big.bin"
rm -f "$fat" "$exfat"
truncate -s 64M "$fat"
mkfs.vfat -F 32 -n VEXAFAT "$fat" > /dev/null
mcopy -s -i "$fat" "$content"/* ::/
truncate -s 32M "$exfat"
mkfs.exfat -q -L VEXAEXFAT "$exfat" > /dev/null
