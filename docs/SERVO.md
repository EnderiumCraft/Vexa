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

- [x] **M2: Servo's leaf crates.** These all run in the VM: `getrandom`
      (0.2 and 0.3), `mio` 1.2.4 on its `poll(2)` selector with the pipe waker, `socket2`,
      `tokio` 1.53 (runtime, timer, TCP), `rustls` 0.23 with `ring` 0.17 as the crypto
      provider (a TLS handshake over loopback, with a self-signed certificate made in the
      probe), and `hyper` 1.12's HTTP/1 client and server over loopback. rustls trusts the
      system's CA bundle, `/etc/ssl/certs/ca-certificates.crt` (121 roots): a self-signed
      server is refused, and a leaf signed by a CA added to those roots is accepted. The
      `poll` limit is gone (libvexa sizes its handle array to the call, up to the kernel's
      256), `dlopen` and `dlsym` load and resolve, and `SO_RCVTIMEO` gives a real read
      timeout. Still open: a real site. This sandbox gives the VM no DNS answer, so the
      HTTPS probe reports the failed lookup instead of a result.

      ```sh
      PATH=$PWD/build/sdk/vexa-sdk/bin:$PATH sh tools/rust/build-std-vexa.sh WORKDIR
      sh tools/rust/build-crates-vexa.sh WORKDIR
      ```

      The crates get their Vexa edits from [`tools/rust/crates-vexa-glue.py`](../tools/rust/crates-vexa-glue.py).
      The probes are [`crates-probe.rs`](../tools/rust/crates-probe.rs) (mio),
      [`tokio-probe.rs`](../tools/rust/tokio-probe.rs), [`tls-probe.rs`](../tools/rust/tls-probe.rs),
      [`hyper-probe.rs`](../tools/rust/hyper-probe.rs) and
      [`https-probe.rs`](../tools/rust/https-probe.rs) and
      [`native-probe.rs`](../tools/rust/native-probe.rs) (`poll` over 120 descriptors,
      `dlopen` and `dlsym`, and a read timeout).

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
- **No `epoll`, and `poll` had a 64-handle limit.** libvexa has `poll` and no `epoll`, so
  mio uses its `poll(2)` selector, with the pipe waker. mio hands `poll` every registered
  descriptor in one call, and libvexa's `poll` used a fixed 64-slot array, so more than 64
  registered sockets failed. It now sizes the array to the call, and the kernel's limit of
  256 handles is the bound.
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
  ignores it, so these names compile, but options such as `IP_HDRINCL` and the multicast
  interface have no effect yet. `SO_RCVTIMEO` and `SO_SNDTIMEO` are real: they go to the
  kernel through `vx_socket_timeout`, and the read timeout reads back. The others stay
  accept-and-ignore on purpose. busybox's `ping` and `udhcpc` check that `setsockopt`
  succeeds, so an error for every unknown option would break them.
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
- **SpiderMonkey compiles for Vexa, with guards.** `mozjs_sys` (version 153) configures
  SpiderMonkey per OS, and Vexa was not one of them. The patch in
  [`third_party/mozjs-vexa.patch`](../third_party/mozjs-vexa.patch) adds the target, and the
  build (`libjs_static.a`, x86-64 ELF) runs from the script on a clean tree. What it needed:
  - **No iostreams, locales, wide characters or `<locale.h>` in the SDK's libc++, by design**
    ([`docs/SDK.md`](SDK.md)). Declaration-only uses take `<iosfwd>`; bodies that write to a
    stream, and the trace printers, are left out on Vexa. Conversions that went through the
    C library's locale use UTF-8, which is what Vexa's only locale is. Default locale is `"und"`.
  - **Clang 19 on the host.** Configure rejects clang 18. Set `HOST_CC`/`HOST_CXX` (the
    script's defaults are clang-19), and `VEXA_HOST_CC`/`VEXA_HOST_CXX`: without them the SDK's
    `vexa-cc` falls back to the system `cc`, which is gcc here.
  - **Headers from the crates and the repo**, put on `CPPFLAGS` the way `mozjs_sys` does:
    `encoding_c_mem`, `mozjs_normalizer_glue` (its header is made by cbindgen, and its
    `<ostream>` include is changed to `<iosfwd>`), and zlib from `build/zlib-native`.
  - **Off on purpose:** the JIT (`--disable-jit`), the intl API (`--without-intl-api`), and
    the wasm integer-GEMM feature (`--disable-wasm-moz-intgemm`, which Mozilla keeps out of
    release builds without a security review).
  - **Platform stand-ins:** the GC's soft decommit keeps the pages and reports success (libvexa
    has no `madvise`); the page-fault count is zero (no `getrusage`); the mozglue SIGBUS
    handler is a no-op (Vexa's signals carry no `siginfo`); `gettid` is libvexa's thread id;
    and `NativeStack` gets its stack bounds from `pthread_getattr_np`, which libvexa now has.
  - **Runs in the VM.** [`tools/mozjs/vexa-js-test.cpp`](../tools/mozjs/vexa-js-test.cpp) starts
    a runtime and a global, runs a script that needs a GC and calls
    `String.prototype.normalize` (through the Rust normalizer glue), and checks the result.
    It prints `js runtime check: pass` with no faults. The binary comes from
    `tools/mozjs/build-vexa-js-test.py`. The C++ side is checked from a clean tree.
  - **Gaps the test needed, now in libvexa:** `__stack_chk_fail` (SpiderMonkey is built with
    stack protection); `fork`, `setsid`, `setpgid`, `chroot`, `mkfifo`, `linkat` and
    `fdopendir`, which fail with `ENOSYS` (Rust's std references them, and a static link pulls
    them in); and an empty-stack `_Unwind_*` set, since Vexa has no unwinder yet.
  - **The Rust side runs in the VM.** `mozjs` 0.26.8, the high-level crate, on `mozjs_sys`
    153.4.0-0 starts a runtime, evaluates the same script as the C++ test, and prints
    `rust js check: pass`. [`tools/mozjs/rust-test`](../tools/mozjs/rust-test) is the program,
    and [`tools/mozjs/build-rust-vexa.sh`](../tools/mozjs/build-rust-vexa.sh) builds it from
    the registry crates with [`third_party/mozjs-sys-vexa.patch`](../third_party/mozjs-sys-vexa.patch)
    on top of the SpiderMonkey patch. What the Rust side needed:
    - `libc` 0.2.190 with Vexa's module. The crates.io libc has none, and the sysroot's
      libc is 0.2.178, so the crates get the 0.2.190 patch.
    - `-D__vexa__=1` for bindgen, which runs clang directly and misses the define that
      `vexa-c++` makes.
    - The build script's configure flags pass `--disable-wasm-moz-intgemm` (as the C++
      build does), and the normalizer header gets the same `<ostream>` to `<iosfwd>` change.
    - A `malloc` size stub: libvexa has no `malloc_usable_size`, so the size reports read 0.
    - `libc++abi` (operator new/delete and the guards) in the binary's link group. Rust
      orders `libc++` and `libc++abi` the wrong way round for static archives, and a
      dependency's `rustc-link-arg` does not reach the binary, so the binary's build script
      adds the group.
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
2. **M2, Servo's leaf crates.** Done, bar a real HTTPS fetch. The crates above all run in
   the VM, with std's `Instant` for time and rustls on `/etc/ssl/certs/ca-certificates.crt`.
   A real site needs a VM that resolves names, and this sandbox does not give one.
3. **M3, SpiderMonkey on Vexa.** Done. The engine builds for `x86_64-unknown-vexa`
   (`tools/mozjs/build-mozjs-vexa.sh`), links with the Rust glue, and runs a script with a GC
   in the VM. The Rust bindings build too (`tools/mozjs/build-rust-vexa.sh`), and the Rust
   program runs the same script in the VM and passes. The JIT and intl stay off for now.
4. **M4, GL for WebRender.** Done. Mesa's softpipe through OSMesa gives OpenGL 3.3 core, and
   WebRender needs 3.2: [`tests/gl/core-gl-test.c`](../tests/gl/core-gl-test.c) checks the core
   profile in the VM. surfman had no OSMesa backend, so
   [`third_party/surfman-vexa.patch`](../third_party/surfman-vexa.patch) adds one (selected on
   Vexa), and WebRender 0.70 creates its renderer on that context, draws a rectangle, and gets
   the right pixels back. The test is [`tools/webrender/webrender-test`](../tools/webrender/webrender-test),
   built by [`tools/webrender/build-webrender-vexa.sh`](../tools/webrender/build-webrender-vexa.sh),
   and it prints `webrender-test: passed` in the VM.
5. **M5, an embedder.** Two parts are done.
   - **A local file, offscreen.** [`servo-render`](../tools/servo/servo-render) loads
     [`tests/servo/render-test.html`](../tests/servo/render-test.html) through `file://`, paints
     it, reads the frame back through `SoftwareRenderingContext`, and checks the pixels (orange,
     with a blue square in the top-left corner). It prints `servo-render: passed` in the VM.
   - **A window.** [`servo-window`](../tools/servo/servo-render/src/bin/servo-window.rs) opens a
     libvexa window and paints the page into it. The pointer (press and wheel), the keyboard
     (typed text into a field) and the window's resize go to the `WebView`. The window test,
     [`tools/servo/run-window-test.py`](../tools/servo/run-window-test.py) with
     [`tests/servo/window-test.html`](../tests/servo/window-test.html), drives each through the
     desktop in the VM. The page sets its title on each event, and the program prints each
     title, so the log shows the events arrive: `mousedown 250,215`, `typed hi`, `wheel 40`,
     `scroll 40`, and the resize. Known issue, below.

   Both are built by [`tools/servo/build-servo-vexa.sh`](../tools/servo/build-servo-vexa.sh).
   Servo's crates need [`third_party/servo-vexa.patch`](../third_party/servo-vexa.patch): no
   sandbox (gaol is left out on Vexa, and multiprocess is off), no ICU, ring for TLS and
   WebCrypto, in-process IPC, FreeType fonts, `navigator.platform` as `Vexa`, and inventory's
   constructors, which the default resources need. surfman's OSMesa context is current from its
   creation, because glow reads the GL version before a surface is bound. Still missing: the JIT
   and intl. Backtraces are empty, as there is no unwinder.
   - **Text.** Servo's font list on Vexa ([`servo-fonts`' `vexa` module](../third_party/servo-vexa.patch))
     reads the TrueType and OpenType files in `/share/fonts`, the DejaVu faces that libvexa's
     text uses, through FreeType. A test page's text gets a width, and its render is painted:
     [`tests/servo/font-test.html`](../tests/servo/font-test.html) sets its title to `fonts ok`,
     and the same VM run (`tools/servo/run-render-test.py`) checks that title and the image
     (`Hello from Vexa`, in DejaVu Sans).
   - **Loading over HTTP.** `servo-render` loads an `http://` page through Servo's `net` crate.
     [`tools/servo/run-net-test.py`](../tools/servo/run-net-test.py) serves `tests/servo` on the
     host, and the VM loads `http://10.0.2.2:8765/font-test.html` (10.0.2.2 is the host, from the
     VM's user-mode network); the page's title must be `fonts ok`. The first load failed in
     hyper-util's connector: socket2 reads a socket's flags with `fcntl(F_GETFL)`, and libvexa
     answered `EBADF`, because it stat'd the handle as a file. A socket is not a file, so
     [`libvexa/src/posix.c`](../libvexa/src/posix.c) now reports the socket's own non-blocking
     state. The VM does not resolve names yet, so the test uses the address.
   - **Loading over HTTPS.** [`tools/servo/run-https-test.py`](../tools/servo/run-https-test.py)
     makes a test CA and a certificate for `10.0.2.2`, serves `tests/servo` over TLS, and checks
     two loads in the VM. Without the test CA the page is refused (`UnknownIssuer`, which Servo
     shows as the title `Certificate error`). With `SSL_CERT_FILE` set to the system bundle plus
     the test CA, the page loads and its title is `fonts ok`. Servo's default verifier reads the
     system bundle, `/etc/ssl/certs/ca-certificates.crt`, through `rustls-native-certs`, which
     honors `SSL_CERT_FILE`, so Servo needed no change. Public sites still need name resolution,
     which the VM does not have yet.

   **Known issue, parked: a resized window paints a narrow frame.** After a resize the page is laid
   out at the new size, and `read_to_image` returns the new size, but the pixels past the old 800
   columns come back white. What is ruled out: OSMesa and Mesa. A standalone OSMesa program that
   resizes from 800x600 to 1278x744 draws the full width. What is found: the first paint after the
   resize still composites the old frame, with WebRender's viewport at the old 800x600 (offset to
   y=144). Asking WebRender for a frame on resize (`generate_frame` in `servo-paint`'s
   `resize_rendering_context`) makes the viewport the full size, but the content past x=800 is
   still white, and repainting for two seconds does not change it. So the new display list does
   not reach the composited frame. Not found: why. The debugging changes are not in the repo.
6. **M6, integration.** An app bundle, a desktop entry, tests and docs, and the sandbox
   decision (`gaol` does not support Vexa).

## Next step

The browser, which is the goal:

- An address bar, and names in the VM (a resolver, or `/etc/hosts`), so `https` works on real
  sites and not only by address.
- Then M6: an app bundle, a desktop entry, tests, and the docs.
- Parked: the resize (above). Next time, look at why the page's new display list does not reach
  the composited frame, in Servo's painter and WebRender's frame building.
