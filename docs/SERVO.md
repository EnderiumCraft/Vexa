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

      Not checked yet: threads, files, and environment variables from a `std` program.
      The probe only prints.

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
- **No `epoll`.** libvexa has `poll` only, and `vx_poll` takes at most 64 handles per call.
  Servo's networking (tokio, and mio underneath it) needs an event backend, so either a
  `poll`-based backend or an `epoll` shim in libvexa.
- **No `getrandom`, no unwinder, no `readv`/`writev`/`ioctl`.** Covered above. The libc
  shims and the std edits in the script are the workarounds, not the end state.
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

1. **M1, `std` on Vexa.** Done for the probe. Still to check: a thread, a file, and an
   environment variable from a `std` program, in the VM.
2. **M2, Servo's leaf crates.** Build the crates Servo needs below the engine: `getrandom`
   (or its `/dev/urandom` fallback), time, `dlopen`, `mio`/`tokio` (on a `poll` or `epoll`
   backend), `rustls` and `hyper`.
3. **M3, SpiderMonkey on Vexa.** Port the `mozjs_sys` build (a target config for Vexa, and
   the C++ runtime from the libc++ work in Phase 67).
4. **M4, GL for WebRender.** Give surfman a software GL context on Vexa, from OSMesa
   (softpipe). Check first that softpipe gives the GL version WebRender requires.
5. **M5, an embedder.** A Vexa app that hosts Servo: a window through libvexa's GUI, input,
   loading pages through `net`, painting the frame. Runs in the VM.
6. **M6, integration.** An app bundle, a desktop entry, tests and docs, and the sandbox
   decision (`gaol` does not support Vexa).

## Next step

M1's last check: a `std` program that starts threads, reads and writes a file, and reads an
environment variable, run in the VM. Then M2: the leaf crates Servo needs, starting with
`getrandom` and `mio`, which decide what libvexa has to grow (an `epoll` shim, or a
`poll`-based backend).
