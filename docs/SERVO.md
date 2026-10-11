# Servo on Vexa: the plan

Servo (the Rust web engine) as Vexa's own browser, built natively for Vexa rather than
run through the Linux subsystem. This is a multi-session project. This file records where
it stands, what the probes found, and the milestones from here. Update it as each one
lands.

## Status

- [x] **M0: Rust runs on Vexa.** A `#![no_std]` Rust program, built for the bare-metal
      target `x86_64-unknown-none` and linked with `vexa-cc -static`, logs its message
      through libvexa's `vx_log` when run in the VM.

      ```sh
      rustup target add x86_64-unknown-none
      rustc --target x86_64-unknown-none --crate-type=staticlib -C panic=abort -O hello.rs -o libhello.a
      vexa-cc -static libhello.a -o hello
      ```

- [x] **M1: Rust `std` runs on Vexa.** A program that uses `println!` builds against
      Rust's standard library for `x86_64-unknown-vexa`, links, and prints
      `hello from std on Vexa` when run in the VM. It is dynamically linked against
      `/lib/vexa-ld.so`, as any Vexa program is.

      The whole build is one script, [`tools/rust/build-std-vexa.sh`](../tools/rust/build-std-vexa.sh).
      It needs rustup with the stable toolchain and its `rust-src` component, Python 3,
      `patch`, and network access to crates.io, and it puts the SDK's `bin` on `PATH` for
      the linker:

      ```sh
      PATH=$PWD/build/sdk/vexa-sdk/bin:$PATH sh tools/rust/build-std-vexa.sh WORKDIR
      ```

      It fetches libc 0.2.178, applies [`third_party/libc-vexa.patch`](../third_party/libc-vexa.patch),
      generates the declarations ([`tools/rust/gen-vexa-libc.py`](../tools/rust/gen-vexa-libc.py)),
      copies the stable `rust-src` and points its libc dependencies at that copy, applies the
      Vexa edits to `std` ([`tools/rust/std-vexa-glue.py`](../tools/rust/std-vexa-glue.py)),
      links a toolchain whose `rustc` uses the patched sysroot, and builds a probe program.
      The target is [`tools/rust/x86_64-unknown-vexa.json`](../tools/rust/x86_64-unknown-vexa.json).

      What Vexa does differently from Linux, where `std` had to be told:
      - **Random bytes** come from `/dev/urandom`, which the kernel's entropy source backs.
      - **Backtraces** are the no-op ones: Vexa has no unwinder yet.
      - **Thread names** are not set (Vexa has none).
      - **`current_exe`** reads `/proc/self/exe`.
      - **Vectored I/O** (`readv`, `writev`) is a loop over `read` and `write`.
      - **`utimensat`, `futimens`** report `ENOSYS`, and **`ioctl`** is declared but has no
        implementation in libvexa yet, so a program that calls it fails to link.
      - **`getpwuid_r`** is built over libvexa's `getpwuid`.

      Checked in the VM too: a `std` program starts a thread (which returns 42 to its
      `join`), writes and reads back a file under `/home/vexa`, reads `PATH` from the
      environment, and counts its arguments. All four print as expected, and the
      serial log has no faults. The check program is not in the repo; the build script's
      probe only prints.

- [ ] **M2: Servo's leaf crates (nearly done).** These all run in the VM: `getrandom`
      (0.2 and 0.3), `mio` 1.2.4 on its `poll(2)` selector with the pipe waker, `socket2`,
      `tokio` 1.53 (runtime, timer, TCP), `rustls` 0.23 with `ring` 0.17 as the crypto
      provider (a TLS handshake over loopback, with a self-signed certificate made in the
      probe), and `hyper` 1.12's HTTP/1 client and server over loopback. Still open: how
      rustls trusts real sites, the 64-handle `poll` limit, and `dlopen`, which is not probed.

      ```sh
      PATH=$PWD/build/sdk/vexa-sdk/bin:$PATH sh tools/rust/build-std-vexa.sh WORKDIR
      sh tools/rust/build-crates-vexa.sh WORKDIR
      ```

      The crates get their Vexa edits from [`tools/rust/crates-vexa-glue.py`](../tools/rust/crates-vexa-glue.py).
      The probes are [`crates-probe.rs`](../tools/rust/crates-probe.rs) (mio),
      [`tokio-probe.rs`](../tools/rust/tokio-probe.rs), [`tls-probe.rs`](../tools/rust/tls-probe.rs)
      and [`hyper-probe.rs`](../tools/rust/hyper-probe.rs).

## What the probes found

- **libc had no `vexa` module.** The libc crate (0.2.178, what `std` depends on) failed with
  `unresolved import unistd` for an unknown OS. That is now solved (see M1 above).
- **libvexa's ABI is only partly Linux's.** errno values and the `O_*` flags match Linux
  x86_64 (`ENOENT` 2, `EAGAIN` 11, `O_CREAT` 0100, `O_NONBLOCK` 04000), so constants can
  be shared. `struct stat` does not match: libvexa orders the fields differently from Linux's
  x86_64 layout. So libc's Linux module can't be reused as it is. The Vexa module takes its
  layouts from libvexa's headers, and `std` reuses the Linux module's Unix code on top of it.
- **libvexa defines some constants with Linux's values, and not all of them.** Where it lacks
  a value libc needs (`O_NOFOLLOW`, `EWOULDBLOCK`, the `IP_*` options and others), the Vexa
  module uses Linux's value. Those should become Vexa's own values once libvexa defines them.
- **No `epoll`, and `poll` takes at most 64 handles.** libvexa has `poll` and no `epoll`, so
  mio uses its `poll(2)` selector, with the pipe waker. mio hands `poll` every registered
  descriptor in one call, so more than 64 registered sockets fail with `EINVAL` until
  libvexa raises the limit or gets an `epoll` shim.
- **Pipes had no non-blocking mode.** The kernel's pipes block, and libvexa tracked
  `O_NONBLOCK` only for sockets. A read on an empty pipe therefore blocked where Linux
  returns `EAGAIN`, and mio's waker, which reads its pipe until a read fails, hung on its
  first wake. libvexa now checks readiness with a zero-timeout `poll` before a non-blocking
  read or write on a pipe. The check can race if two threads read one pipe; mio has one
  reader, so it is enough for now.
- **libc's Vexa module lacked names that `socket2` uses, and `tokio` needed a `peer_cred`
  fallback.** The names are in the hand part of the libc patch, with Linux's values, and
  `ip_mreqn` and `ip_mreq_source` have Linux's layouts. `socket2`'s `IovLen` takes its
  `usize` branch, since libvexa's `msg_iovlen` is a `size_t`. `tokio`'s
  `UnixStream::peer_cred` returns `Unsupported`, because libvexa has no `SO_PEERCRED`.
  libvexa's `setsockopt` accepts any option at the socket, IP, IPv6 and TCP levels and
  ignores it. So these names compile, but options such as `IP_HDRINCL` and the multicast
  interface have no effect yet.
- **Non-blocking listeners blocked in `accept`.** The kernel's `accept` waits for a
  connection even on a non-blocking listener, so an accept loop hung after its first
  connection. libvexa's `accept` now checks readiness first, as its pipe reads do, and
  returns `EAGAIN` when no connection is waiting.
- **`ioctl(FIOCLEX)` reached the device layer.** `std`'s socket code sets close-on-exec with
  it, and libvexa answered `ENOTTY`, so `TcpListener::bind` failed. libvexa now takes
  `FIOCLEX` and `FIONCLEX` as no-ops, as it does `F_SETFD`: descriptors have no
  close-on-exec flag yet. Its other `ioctl` requests are `FIONBIO`, `FIONREAD` and
  `SIOCGIFCONF`; the rest go to the device.
- **mio left client sockets blocking.** Its socket setup makes a socket non-blocking with
  `SOCK_NONBLOCK` on Linux-like systems and with `fcntl` on the systems in a second list.
  Vexa was in neither, so a client's first read blocked the single-threaded runtime. The
  glue adds Vexa to the second list.
- **ring needs three edits for Vexa.** Its `SystemRandom` takes entropy through `getrandom`
  only on the systems it lists, its assembly is built only for the systems in `LINUX_ABI`,
  and both lists now include Vexa. Without the assembly, its crypto routines are missing
  at link time.
- **No `getrandom(2)`, no unwinder.** `getrandom` is covered by `/dev/urandom`, the same as
  in M1. The libc shims and the std edits in the script are the workarounds, not the end
  state.
- **SpiderMonkey has no Vexa target.** `mozjs_sys` (version 153) configures SpiderMonkey per
  OS, and Vexa is not one of them. Its port is the biggest unknown.
- **Servo's dependencies** (servo 0.7.0): Stylo (CSS), WebRender and surfman (GL), `mozjs`
  (JavaScript), tokio-based networking, and optional GStreamer media (`servo-media-dummy`
  is the stand-in). The `gaol` sandbox is compiled only on platforms it lists, so Vexa needs
  a decision there.
- **Host limits.** The build machine has 4 cores, 15 GB of RAM and about 22 GB of free disk.
  A full Servo build likely needs more space than that, so plan big builds for a larger
  machine or CI before starting them.

## Milestones

1. **M1, `std` on Vexa.** Done. The probe prints, and a `std` program has run threads, a
   file, and an environment variable in the VM.
2. **M2, Servo's leaf crates.** Nearly done. The crates above all run in the VM, with
   std's `Instant` for time. Still open: rustls needs a trust store for real sites (a
   `webpki-roots` build embeds Mozilla's roots, so no file read is needed; or read
   `/etc/ssl/certs/ca-certificates.crt`, which the TLS phase installs), the 64-handle `poll`
   limit, and `dlopen`, which libvexa
   has but nothing has probed.
3. **M3, SpiderMonkey on Vexa.** Port the `mozjs_sys` build (a target config for Vexa, and
   the C++ runtime from the libc++ work in Phase 67).
4. **M4, GL for WebRender.** Give surfman a software GL context on Vexa, from OSMesa
   (softpipe). Check first that softpipe gives the GL version WebRender requires.
5. **M5, an embedder.** A Vexa app that hosts Servo: a window through libvexa's GUI, input,
   loading pages through `net`, painting the frame. Runs in the VM.
6. **M6, integration.** An app bundle, a desktop entry, tests and docs, and the sandbox
   decision (`gaol` does not support Vexa).

## Next step

Close out M2: choose the trust store for rustls, probe `dlopen`, and fix the 64-handle `poll`
limit (a server with more than 64 connections will fail). Also decide what `setsockopt`
should do with options it accepts and ignores. Then M3, SpiderMonkey, which needs its build
read before anything else can be sized.
