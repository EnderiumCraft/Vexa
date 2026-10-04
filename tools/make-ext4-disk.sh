#!/bin/sh
# Builds an ext4 disk image the way Linux makes them (mkfs.ext4's defaults:
# extents, 64-bit group descriptors, flex_bg, metadata checksums), for testing
# Vexa's (read-only) ext4 support: a file, a symbolic link, a directory big
# enough to get a hashed index, and a sparse file whose extents need an index
# block. Usage: tools/make-ext4-disk.sh OUTPUT
set -eu
out=$1
content=$(mktemp -d)
trap 'rm -rf "$content"' EXIT
echo "Hello from an ext4 disk!" > "$content/hello.txt"
ln -s hello.txt "$content/link"
mkdir -p "$content/dir/many"
i=1
while [ $i -le 400 ]; do
    echo $i > "$content/dir/many/file-with-a-longer-name-$i.txt"
    i=$((i + 1))
done
# 40 blocks of a letter each, with a hole after each: 40 extents.
python3 -c "
f = open('$content/sparse.bin', 'wb')
for i in range(40):
    f.seek(i * 8192)
    f.write(bytes([65 + i % 26]) * 4096)
"
rm -f "$out"
mkfs.ext4 -q -F -d "$content" -L vexa-ext4 "$out" 32M
e2fsck -fyD "$out" > /dev/null || [ $? -le 1 ]  # (-D: index the big directory.)
