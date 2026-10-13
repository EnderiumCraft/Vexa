#!/bin/sh
# Builds Rust's std for x86_64-unknown-vexa from the stock rust-src, with Vexa's libc
# (third_party/libc-vexa.patch, plus the declarations gen-vexa-libc.py makes from
# libvexa's headers) and Vexa's edits to std (std-vexa-glue.py). Then it builds a small
# probe program against them.
#
#   build-std-vexa.sh WORKDIR
#
# Needs: rustup with the stable toolchain (or the one named in VEXA_RUST_BASE) and its rust-src
# component, python3, patch,
# cargo with network access to crates.io (for the libc 0.2.178 source), and vexa-cc on
# PATH (the SDK's bin directory) for the target's linker.
#
# WORKDIR is recreated on each run. The linked toolchain is named vexa-std.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
tc_name=vexa-std
stable=$(rustup run "${VEXA_RUST_BASE:-stable}" rustc --print sysroot)
src=$stable/lib/rustlib/src/rust/library

echo "== libc 0.2.178 with the Vexa patch and declarations"
rm -rf "$work/fetch" "$work/libc"
mkdir -p "$work/fetch/src"
cat > "$work/fetch/Cargo.toml" <<'EOF'
[package]
name = "fetch"
version = "0.0.0"
edition = "2021"

[dependencies]
libc = "=0.2.178"
EOF
echo 'fn main() {}' > "$work/fetch/src/main.rs"
(cd "$work/fetch" && cargo fetch -q)
libc_src=$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/libc-0.2.178 | head -1)
cp -r "$libc_src" "$work/libc"
chmod -R u+w "$work/libc"
patch -p2 -s -d "$work/libc" < "$repo/third_party/libc-vexa.patch"
python3 "$here/gen-vexa-libc.py" "$repo/libvexa/include" \
    "$work/libc/src/unix/vexa" "$work/libc/src/unix/mod.rs"
python3 "$here/gen-vexa-libc-extra.py" "$work/libc/src" \
    "$work/libc/src/unix/vexa/extra.rs"

echo "== the sysroot's library tree: libc from the copy above, then Vexa's std edits"
rm -rf "$work/library"
cp -r "$src" "$work/library"
chmod -R u+w "$work/library"
python3 - "$work/library" "$work/libc" <<'PY'
import glob
import re
import sys

library, libc = sys.argv[1], sys.argv[2]
for path in glob.glob(library + "/**/Cargo.toml", recursive=True):
    text = open(path).read()
    new = re.sub(r'^(libc = \{[^}]*?)version = "[^"]*"',
                 lambda m: m.group(1) + 'path = "%s"' % libc, text, flags=re.M)
    if new != text:
        open(path, "w").write(new)

# libc is a local package now: its registry source and checksum go.
path = library + "/Cargo.lock"
lines = open(path).read().split("\n")
out, i = [], 0
while i < len(lines):
    out.append(lines[i])
    if lines[i] == 'name = "libc"' and i + 1 < len(lines) and lines[i + 1].startswith("version = "):
        out.append(lines[i + 1])
        i += 2
        while i < len(lines) and (lines[i].startswith("source = ") or lines[i].startswith("checksum = ")):
            i += 1
        continue
    i += 1
open(path, "w").write("\n".join(out))
PY
python3 "$here/std-vexa-glue.py" "$work/library"

echo "== the toolchain: the stable binaries, with the patched library as their sysroot"
tc=$work/toolchain
rm -rf "$tc"
mkdir -p "$tc/bin" "$tc/lib/rustlib/src/rust"
for f in "$stable"/bin/*; do
    n=$(basename "$f")
    [ "$n" = rustc ] || ln -s "$f" "$tc/bin/$n"
done
# rustc finds its sysroot from its own path, which a symlink would resolve back to the
# stable toolchain. The wrapper passes the sysroot explicitly.
printf '#!/bin/sh\nexec %s/bin/rustc --sysroot %s "$@"\n' "$stable" "$tc" > "$tc/bin/rustc"
chmod +x "$tc/bin/rustc"
for f in "$stable"/lib/*; do
    n=$(basename "$f")
    [ "$n" = rustlib ] || ln -s "$f" "$tc/lib/$n"
done
ln -s "$stable/lib/rustlib/x86_64-unknown-linux-gnu" "$tc/lib/rustlib/x86_64-unknown-linux-gnu"
ln -s "$work/library" "$tc/lib/rustlib/src/rust/library"
rustup toolchain remove "$tc_name" 2>/dev/null || true
rustup toolchain link "$tc_name" "$tc"

echo "== a probe program, built with std for Vexa"
rm -rf "$work/probe"
mkdir -p "$work/probe/src"
cat > "$work/probe/Cargo.toml" <<'EOF'
[package]
name = "probe"
version = "0.0.0"
edition = "2021"

[dependencies]
EOF
echo 'fn main() { println!("hello from std on Vexa"); }' > "$work/probe/src/main.rs"
cd "$work/probe"
RUSTC_BOOTSTRAP=1 cargo +"$tc_name" build --target "$repo/tools/rust/x86_64-unknown-vexa.json" \
    -Zbuild-std=core,alloc,std,panic_abort
