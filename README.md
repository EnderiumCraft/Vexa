# Vexa

Vexa is a hobby operating system for x86_64, written from scratch in C.
The long-term goal is to run **Mozilla Firefox**.

Vexa has its own kernel design, its own system call interface and its own C library.
Linux programs such as Firefox run through a separate, optional compatibility subsystem
that sits on top of the Vexa kernel. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

![The Vexa kernel monitor running in QEMU](docs/screenshot.png)

## Status

Phase 1 (boot and CPU basics) is complete. The kernel:

- boots through the [Limine](https://github.com/limine-bootloader/limine) bootloader (BIOS and UEFI)
- runs in 64-bit long mode as a higher-half kernel
- shows a text console on the screen and mirrors it to the COM1 serial port
- loads its own GDT and IDT and reports CPU exceptions with a register dump
- reads the ACPI tables and sets up the local APIC and I/O APIC
- runs a 1000 Hz timer, calibrated against the PIT
- reads the PS/2 keyboard (US layout, Shift, Caps Lock, Ctrl)
- ends in a small built-in command line, the kernel monitor (`help`, `cpu`,
  `uptime`, `clear`, `reboot`), until it can run real programs

Next up is Phase 2, memory management. See [docs/ROADMAP.md](docs/ROADMAP.md) for the full
plan from here to Firefox.

## Building

You need a Linux host (or WSL) with:

| Tool | Debian/Ubuntu package |
| --- | --- |
| GCC or Clang targeting x86_64 | `build-essential` |
| GNU ld | `binutils` |
| xorriso | `xorriso` |
| QEMU | `qemu-system-x86` |
| git, make | `git`, `make` |
| Python 3 (for `make test`) | `python3` |
| UEFI firmware (for `make test`) | `ovmf` |

```sh
make                # builds build/vexa.iso (fetches Limine on first run)
make run            # boots in QEMU; kernel log appears in your terminal
make run-nographic  # headless boot, serial only (Ctrl-A then X to quit)
make test           # boots in QEMU (BIOS, then UEFI with 4 CPUs), types commands
                    # into the virtual keyboard and checks the replies
make clean
```

## Layout

```
Makefile             build, ISO creation, QEMU and test targets
limine.conf          bootloader menu entry
kernel/
  linker.ld          higher-half kernel link script
  include/limine.h   Limine boot protocol header (0BSD, vendored)
  include/vexa/      kernel headers
  src/kmain.c        kernel entry point and boot sequence
  src/arch/x86_64/   GDT, IDT, interrupt entry, APIC, timer
  src/core/          early memory mapping, ACPI, kernel monitor
  src/dev/           serial, framebuffer, text console, font, PS/2 keyboard
  src/lib/           string functions, kprintf, panic
tools/
  bdf2c.py           converts a BDF bitmap font into the console font table
  qemu-smoke-test.py boot test used by `make test`
docs/
  ARCHITECTURE.md    how the kernel, native interface and Linux subsystem fit together
  ROADMAP.md         the plan, phase by phase
```

The console font is [Spleen](https://github.com/fcambus/spleen) 8x16 by Frederic Cambus
(BSD 2-Clause license, reproduced in `kernel/src/dev/font.c`).
