#!/bin/sh
# Builds the crates Servo needs below the engine for Vexa, and probes that use them.
# Run build-std-vexa.sh first: this uses the libc and the vexa-std toolchain it makes.
#
#   build-crates-vexa.sh WORKDIR
#
# The crates are mio 1.2.4, socket2 0.6.5 and getrandom 0.2.17 and 0.3.4. Vexa's edits to
# them are tools/rust/crates-vexa-glue.py. Two probes use them:
#   crates-probe.rs  mio's event loop (its poll(2) selector and pipe waker), a TCP connection
#                    over loopback, and random bytes from both getrandom versions
#   tokio-probe.rs   tokio's current-thread runtime, a timer, and a TCP connection over
#                    loopback, on the same mio and socket2
#
# WORKDIR is the directory build-std-vexa.sh made. Its crates/ and probes are recreated on
# each run.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(cd "$1" && pwd)
tc_name=vexa-std
registry=$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/ | head -1)

if [ ! -d "$work/libc" ] || [ ! -d "$work/toolchain" ]; then
    echo "build-crates-vexa: $work has no libc or toolchain; run build-std-vexa.sh first" >&2
    exit 1
fi

echo "== mio, socket2 and getrandom, with Vexa's edits"
rm -rf "$work/fetch-crates" "$work/crates"
mkdir -p "$work/fetch-crates/src" "$work/crates"
cat > "$work/fetch-crates/Cargo.toml" <<'EOF'
[package]
name = "fetch-crates"
version = "0.0.0"
edition = "2021"

[dependencies]
mio = { version = "=1.2.4", features = ["os-poll", "net"] }
socket2 = "=0.6.5"
tokio = "=1.53.2"
getrandom = "=0.2.17"
getrandom03 = { package = "getrandom", version = "=0.3.4" }
EOF
echo 'fn main() {}' > "$work/fetch-crates/src/main.rs"
(cd "$work/fetch-crates" && cargo fetch -q)
for c in mio-1.2.4 socket2-0.6.5 tokio-1.53.2 getrandom-0.2.17 getrandom-0.3.4; do
    cp -r "$(ls -d "$registry"/"$c" | head -1)" "$work/crates/$c"
done
chmod -R u+w "$work/crates"
python3 "$here/crates-vexa-glue.py" "$work/crates"

# The [patch] entries point the crates at the local copies, and the libc they use at the
# patched one from build-std-vexa.sh. Each probe lists the entries it uses: an unused
# patch only makes cargo warn.
patch_common() {
    cat <<EOF
libc = { path = "$work/libc" }
mio = { path = "$work/crates/mio-1.2.4" }
socket2 = { path = "$work/crates/socket2-0.6.5" }
EOF
}

echo "== probe 1: mio's event loop, TCP over loopback, and random bytes from both getrandom"
rm -rf "$work/probe-crates"
mkdir -p "$work/probe-crates/src"
cat > "$work/probe-crates/Cargo.toml" <<EOF
[package]
name = "probe-crates"
version = "0.0.0"
edition = "2021"

[dependencies]
mio = { version = "1.2.4", features = ["os-poll", "net"] }
getrandom02 = { package = "getrandom", version = "0.2" }
getrandom03 = { package = "getrandom", version = "0.3" }

[patch.crates-io]
$(patch_common)
getrandom02 = { package = "getrandom", path = "$work/crates/getrandom-0.2.17" }
getrandom03 = { package = "getrandom", path = "$work/crates/getrandom-0.3.4" }
EOF
cp "$here/crates-probe.rs" "$work/probe-crates/src/main.rs"
cd "$work/probe-crates"
PATH="$repo/build/sdk/vexa-sdk/bin:$PATH" RUSTC_BOOTSTRAP=1 cargo +"$tc_name" build \
    --target "$repo/tools/rust/x86_64-unknown-vexa.json" -Zbuild-std=core,alloc,std,panic_abort

echo "== probe 2: tokio's runtime, a timer, and TCP over loopback"
rm -rf "$work/probe-tokio"
mkdir -p "$work/probe-tokio/src"
cat > "$work/probe-tokio/Cargo.toml" <<EOF
[package]
name = "probe-tokio"
version = "0.0.0"
edition = "2021"

[dependencies]
tokio = { version = "=1.53.2", features = ["rt", "net", "time", "io-util", "macros"] }

[patch.crates-io]
$(patch_common)
tokio = { path = "$work/crates/tokio-1.53.2" }
EOF
cp "$here/tokio-probe.rs" "$work/probe-tokio/src/main.rs"
cd "$work/probe-tokio"
PATH="$repo/build/sdk/vexa-sdk/bin:$PATH" RUSTC_BOOTSTRAP=1 cargo +"$tc_name" build \
    --target "$repo/tools/rust/x86_64-unknown-vexa.json" -Zbuild-std=core,alloc,std,panic_abort
