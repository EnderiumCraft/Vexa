# Servo on Vexa: the plan

Servo (the Rust web engine) as Vexa's own browser, built natively for Vexa rather than
run through the Linux subsystem. This is a multi-session project. This file records where
it stands, what the probes found, and the milestones from here. Update it as each one
lands.

## Status

- [x] **M0: Rust runs on Vexa.** A `#![no_std]` Rust program, built for the bare-metal
      target `x86_64-unknown-none` and linked with `vexa-cc -static`, logs its message
      through libvexa's `vx_log` when run in the VM.
      Build it with:

      ```sh
      rustup target add x86_64-unknown-none
      rustc --target x86_64-unknown-none --crate-type=staticlib -C panic=abort -O hello.rs -o libhello.a
      vexa-cc -static libhello.a -o hello
      ```

- [ ] **M1: Rust `std` for Vexa.** The target definition is in
      [`tools/rust/x86_64-unknown-vexa.json`](../tools/rust/x86_64-unknown-vexa.json)
      (`os` = `vexa`, `target-family` = `unix`, `panic` = `abort`). Building `std` for it
      with `-Zbuild-std` gets as far as libc and stops there (see below).

## What the probes found

- **libc has no `vexa` module.** The libc crate (0.2.178, what `std` depends on) fails with
  `unresolved import unistd` for an unknown OS. This is the first real gap.
- **libvexa's ABI is only partly Linux's.** errno values and the `O_*` flags match Linux
  x86_64 (`ENOENT` 2, `EAGAIN` 11, `O_CREAT` 0100, `O_NONBLOCK` 04000), so constants can
  be shared. `struct stat` does not match: libvexa orders the fields differently from Linux's
  x86_64 layout. So libc's Linux module can't be reused as it is. A `vexa` module has to be
  written, with layouts taken from libvexa's headers.
- **No `epoll`.** libvexa has `poll` only, and `vx_poll` takes at most 64 handles per call.
  Servo's networking (tokio, and mio underneath it) needs an event backend, so either a
  `poll`-based backend or an `epoll` shim in libvexa.
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

1. **M1, `std` on Vexa.** Add a `vexa` module to libc (types, constants and the functions
   `std` uses, built on libvexa). Build `std` with `-Zbuild-std` and run a `println!`,
   a thread, a file and an environment variable on Vexa.
2. **M2, Servo's leaf crates.** Build the crates Servo needs below the engine: `getrandom`,
   time, `dlopen`, `mio`/`tokio` (on a `poll` or `epoll` backend), `rustls` and `hyper`.
3. **M3, SpiderMonkey on Vexa.** Port the `mozjs_sys` build (a target config for Vexa, and
   the C++ runtime from the libc++ work in Phase 67).
4. **M4, GL for WebRender.** Give surfman a software GL context on Vexa, from OSMesa
   (softpipe). Check first that softpipe gives the GL version WebRender requires.
5. **M5, an embedder.** A Vexa app that hosts Servo: a window through libvexa's GUI, input,
   loading pages through `net`, painting the frame. Runs in the VM.
6. **M6, integration.** An app bundle, a desktop entry, tests and docs, and the sandbox
   decision (`gaol` does not support Vexa).

## Next step

M1. Start with the `vexa` libc module. It is the first thing the `std` build needs, and it
can be checked by building `std` for the target and running a small program in the VM.
