#!/bin/sh
# Builds the crates Servo needs below the engine for Vexa, and probes that use them.
# Run build-std-vexa.sh first: this uses the libc and the vexa-std toolchain it makes.
#
#   build-crates-vexa.sh WORKDIR
#
# The patched crates are mio 1.2.4, socket2 0.6.5, tokio 1.53.2, ring 0.17.14 and getrandom
# 0.2.17 and 0.3.4. Vexa's edits to them are tools/rust/crates-vexa-glue.py. The probes:
#   crates-probe.rs  mio's event loop (its poll(2) selector and pipe waker), a TCP connection
#                    over loopback, and random bytes from both getrandom versions
#   tokio-probe.rs   tokio's current-thread runtime, a timer, and TCP over loopback
#   tls-probe.rs     rustls over loopback, with ring as its crypto provider and a
#                    self-signed certificate made in the probe
#   hyper-probe.rs   hyper's HTTP/1 client and server over loopback, on tokio
#
# WORKDIR is the directory build-std-vexa.sh made. Its crates/ and probes are recreated on
# each run. The probes share one target directory, so std is built once.
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

echo "== the crates, with Vexa's edits"
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
ring = "=0.17.14"
getrandom = "=0.2.17"
getrandom03 = { package = "getrandom", version = "=0.3.4" }
EOF
echo 'fn main() {}' > "$work/fetch-crates/src/main.rs"
(cd "$work/fetch-crates" && cargo fetch -q)
for c in mio-1.2.4 socket2-0.6.5 tokio-1.53.2 ring-0.17.14 getrandom-0.2.17 getrandom-0.3.4; do
    cp -r "$(ls -d "$registry"/"$c" | head -1)" "$work/crates/$c"
done
chmod -R u+w "$work/crates"
python3 "$here/crates-vexa-glue.py" "$work/crates"

# The [patch] entries point each crate at its local copy, and the libc they use at the
# patched one from build-std-vexa.sh. Cargo warns about a patch the graph doesn't use, so
# each probe lists only what it needs.
patch_libc() {
    echo "libc = { path = \"$work/libc\" }"
}
patch_net() {
    patch_libc
    echo "mio = { path = \"$work/crates/mio-1.2.4\" }"
    echo "socket2 = { path = \"$work/crates/socket2-0.6.5\" }"
}

# Every probe builds into one target directory, so std and the shared crates build once.
export CARGO_TARGET_DIR="$work/target"
build_probe() {
    (cd "$1" && PATH="$repo/build/sdk/vexa-sdk/bin:$PATH" CC_x86_64_unknown_vexa=vexa-cc \
        RUSTC_BOOTSTRAP=1 cargo +"$tc_name" build \
        --target "$repo/tools/rust/x86_64-unknown-vexa.json" \
        -Zbuild-std=core,alloc,std,panic_abort)
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
$(patch_net)
getrandom02 = { package = "getrandom", path = "$work/crates/getrandom-0.2.17" }
getrandom03 = { package = "getrandom", path = "$work/crates/getrandom-0.3.4" }
EOF
cp "$here/crates-probe.rs" "$work/probe-crates/src/main.rs"
build_probe "$work/probe-crates"

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
$(patch_net)
tokio = { path = "$work/crates/tokio-1.53.2" }
EOF
cp "$here/tokio-probe.rs" "$work/probe-tokio/src/main.rs"
build_probe "$work/probe-tokio"

echo "== probe 3: rustls over loopback, with ring"
rm -rf "$work/probe-tls"
mkdir -p "$work/probe-tls/src"
cat > "$work/probe-tls/Cargo.toml" <<EOF
[package]
name = "probe-tls"
version = "0.0.0"
edition = "2021"

[dependencies]
rustls = { version = "=0.23.45", default-features = false, features = ["ring", "std", "tls12"] }
rcgen = { version = "=0.14.10", default-features = false, features = ["ring", "pem"] }

[patch.crates-io]
$(patch_libc)
ring = { path = "$work/crates/ring-0.17.14" }
getrandom02 = { package = "getrandom", path = "$work/crates/getrandom-0.2.17" }
EOF
cp "$here/tls-probe.rs" "$work/probe-tls/src/main.rs"
build_probe "$work/probe-tls"

echo "== probe 4: hyper's HTTP/1 client and server over loopback, on tokio"
rm -rf "$work/probe-hyper"
mkdir -p "$work/probe-hyper/src"
cat > "$work/probe-hyper/Cargo.toml" <<EOF2
[package]
name = "probe-hyper"
version = "0.0.0"
edition = "2021"

[dependencies]
bytes = "=1.12.1"
http-body-util = "=0.1.5"
hyper = { version = "=1.12.0", features = ["server", "client", "http1"] }
hyper-util = { version = "=0.1.21", features = ["tokio"] }
tokio = { version = "=1.53.2", features = ["rt", "net", "time", "io-util", "macros"] }

[patch.crates-io]
$(patch_net)
tokio = { path = "$work/crates/tokio-1.53.2" }
EOF2
cp "$here/hyper-probe.rs" "$work/probe-hyper/src/main.rs"
build_probe "$work/probe-hyper"
