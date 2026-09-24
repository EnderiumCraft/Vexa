# Vexa

Vexa is a hobby operating system for x86_64, written from scratch in C.
The long-term goal is to run **Mozilla Firefox**.

![Vexa booting in QEMU](docs/boot-splash.png)

## Status

Phase 1 (bare-metal boot) is in progress. The kernel currently:

- boots through the [Limine](https://github.com/limine-bootloader/limine) bootloader (BIOS and UEFI)
- runs in 64-bit long mode as a higher-half kernel
- logs to the COM1 serial port with a small `kprintf`
- loads its own GDT and IDT and reports CPU exceptions with a register dump
- reads the physical memory map from the bootloader
- draws a splash screen on the framebuffer

See [docs/ROADMAP.md](docs/ROADMAP.md) for the full plan from here to Firefox.

## Building

You need a Linux host (or WSL) with:

| Tool | Debian/Ubuntu package |
| --- | --- |
| GCC or Clang targeting x86_64 | `build-essential` |
| GNU ld | `binutils` |
| xorriso | `xorriso` |
| QEMU | `qemu-system-x86` |
| git, make | `git`, `make` |

```sh
make                # builds build/vexa.iso (fetches Limine on first run)
make run            # boots in QEMU; kernel log appears in your terminal
make run-nographic  # headless boot, serial only (Ctrl-A then X to quit)
make clean
```

## Layout

```
Makefile             build, ISO creation, QEMU targets
limine.conf          bootloader menu entry
kernel/
  linker.ld          higher-half kernel link script
  include/limine.h   Limine boot protocol header (0BSD, vendored)
  include/vexa/      kernel headers
  src/kmain.c        kernel entry point
  src/arch/x86_64/   GDT, IDT, exception entry stubs
  src/dev/           serial port and framebuffer drivers
  src/lib/           string functions, kprintf, panic
docs/ROADMAP.md      the plan, phase by phase
```
