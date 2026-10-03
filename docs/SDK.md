# The Vexa SDK

The SDK builds native Vexa programs and apps on another machine: an x86-64 Linux
computer with GCC (or Clang). It's libvexa (Vexa's C library, with its POSIX layer),
a compiler wrapper, an app template, and SDL 2, so you can write apps for Vexa or port
ones written for other systems.

Each release has it as `vexa-sdk-<version>.tar.gz`. From a Vexa checkout, `make sdk`
builds it in `build/sdk/vexa-sdk` (and the tarball in `build/`).

```
vexa-sdk/
    bin/vexa-cc          the C compiler, set up for Vexa
    bin/vexa-new-app     starts an app from the template
    bin/sdl2-config      SDL's flags
    include/             libvexa's headers (and SDL2/)
    lib/                 libvexa.so, libvexa.a, crt0.o, libSDL2.a, libSDL2_mixer.a, libSDL2_net.a,
                         pkgconfig/, cmake/
    cmake/vexa.cmake     a CMake toolchain file
    template/            the app template
    examples/sdl-demo/   an SDL program as an app
    licenses/            musl's (libm), SDL's, SDL_mixer's and SDL_net's
```

## A first app

```sh
tar -xzf vexa-sdk-0.26.0.tar.gz
vexa-sdk-0.26.0/bin/vexa-new-app "My App"
cd MyApp
make
```

That makes `build/MyApp.vxapp`, an app bundle. Put it on Vexa (on a disk, see below)
and double-click it in Files, or `open MyApp.vxapp` at the shell; copy it into `/apps`
to have it in the menu and in search.

The template is a window with some text, a button and the keys typed: `src/main.c`
draws it with `<vexa/gui.h>` and handles its events. Its `Info.conf` names the app,
its program and its icon (`Resources/icon.png`, 48x48 with transparency); see "App
bundles" in the [developer guide](DEVELOPER-GUIDE.md) for the other keys (`opens=`,
`menu=`...). The [API reference](API.md) has all of libvexa.

## Getting it onto Vexa

Vexa reads ext2 disks, and mounts each at `/mnt/<disk>`: `/mnt/vda` for a whole virtio
disk, `/mnt/vda1` for its first partition. From Linux, make a disk image with the app
on it and give it to QEMU:

```sh
mkdir disk && cp -R build/MyApp.vxapp disk/
mke2fs -q -t ext2 -d disk myapp.img 16M
qemu-system-x86_64 -M q35 -m 1G -cdrom vexa-0.26.0.iso \
    -drive file=myapp.img,format=raw,if=virtio
```

then `open /mnt/vda/MyApp.vxapp`, or `cp -r /mnt/vda/MyApp.vxapp /apps` to install
it. (The checkout's `make run-disk` keeps a disk image of its own between runs.)

## vexa-cc

`vexa-cc` is the build machine's compiler with what Vexa needs: libvexa's headers
instead of the system's, position-independent code, and a link against `libvexa.so`
for Vexa's loader (`/lib/vexa-ld.so`). Use it like `cc`:

```sh
vexa-cc -O2 -c hello.c
vexa-cc hello.o -o hello          # a program for Vexa
vexa-cc -static hello.o -o hello  # with libvexa linked in
```

- It's C (C11 and GNU extensions); there's no C++ library.
- `-lm`, `-lpthread`, `-lrt`, `-ldl` and `-lc` are accepted, and are empty: it's all in
  libvexa.
- It defines `__vexa__` and `__unix__`, and not `__linux__`.
- Vexa's loader loads only `libvexa.so` (from `/lib`), so other libraries are linked
  in statically (`.a` archives built with `vexa-cc` and `ar`).
- `VEXA_HOST_CC=clang` uses Clang instead of `cc`.

For a Makefile project, `make CC=/path/to/vexa-sdk/bin/vexa-cc` usually does. A
configure script may need to be told it's cross-compiling, and that the program can't
run here, in its own way. With CMake:

```sh
cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/vexa-sdk/cmake/vexa.cmake -B build .
```

## The C library

libvexa has the C standard library and much of POSIX, so ported programs mostly build
as they are:

- **Files**: `open`, `read`, `write`, `lseek`, `close`, `stat`, `mkdir`, `unlink`,
  `rename`, `opendir`/`readdir`, `getcwd`, `chdir`, `pipe`, `poll`, `mmap`, `mkstemp`,
  `realpath`, `symlink`; and all of `<stdio.h>`, `fseek`, `ungetc`, `getline` and
  `scanf` included, with exact float formatting.
- **Threads**: pthreads (mutexes, condition variables, read-write locks, keys, once,
  spin locks), semaphores, and `errno` per thread.
- **Time**: `clock_gettime`, `nanosleep`, `gmtime`/`localtime` (in the time zone set in
  Settings), `mktime`, `strftime`.
- **Math**: `<math.h>` is musl's libm (MIT), complete and accurate.
- **Processes**: `system`, `popen`, `getpid`, `exit`/`atexit`; Vexa's own
  `vx_spawn` and `vx_wait` start programs.
- **Strings and numbers**: `<string.h>`, `<strings.h>`, `strtod`, `<wchar.h>`, UTF-8
  multibyte functions, `getopt`, `setjmp`.

Programs start others with `posix_spawn` (`<spawn.h>`, with file actions for the new
program's standard handles) and wait for them with `waitpid`; `dup` and `dup2` work;
`dlopen` loads shared libraries (`vexa-cc -shared -o libfoo.so foo.c`, from `/lib` or a
path) in dynamically linked programs. What isn't there: `fork` (and `exec*` only
approximately: they start the program, wait for it and exit with its code), user and
group IDs beyond stubs, and locales other than "C". Signal handlers set
with `signal`/`sigaction` run for `raise`; signals from outside can be ignored or stop
the program, as for any Vexa program.

## SDL 2

The SDK has SDL 2 (2.30), built for Vexa: each SDL window is a window on the desktop,
with the keyboard, the mouse, text input, the clipboard, dropped files and window
resizing, and sound plays on `/dev/audio0`. Build with `sdl2-config`, pkg-config
(`lib/pkgconfig/sdl2.pc`) or CMake (`find_package(SDL2)` and `SDL2::SDL2`, with the
toolchain file):

```sh
vexa-cc $(sdl2-config --cflags) game.c $(sdl2-config --libs) -lm -o game
```

`examples/sdl-demo` is an SDL program as a Vexa app (`make` in it, or copy it
somewhere and `make SDK=/path/to/vexa-sdk`).

Drawing is in software: SDL's renderer (the "software" one) or the window's surface
(`SDL_GetWindowSurface`). There's no OpenGL or Vulkan, no joysticks, no relative mouse
mode, and full screen is a window as big as the screen. The pointer can take SDL's
system shapes, but not pictures of its own.

## SDL_mixer

SDL_mixer 2 (2.8) is there too, for sound effects and music: WAV, AIFF and VOC
files, and Ogg Vorbis, MP3 and FLAC music (through the single-file decoders it
carries). Link with `-lSDL2_mixer` (before SDL), pkg-config (`SDL2_mixer`) or CMake
(`find_package(SDL2_mixer)` and `SDL2_mixer::SDL2_mixer`). MIDI music needs a program's
own synthesizer: Doom's (Chocolate Doom, in Vexa's Doom app) uses SDL_mixer's music
hook with an OPL emulator.

## SDL_net

SDL_net 2 (2.2), for TCP and UDP (over libvexa's BSD sockets, which programs can use
directly too: `<sys/socket.h>`, `<netdb.h>` and the rest, IPv4). Link with
`-lSDL2_net`, pkg-config (`SDL2_net`) or CMake (`find_package(SDL2_net)` and
`SDL2_net::SDL2_net`). Doom's network games use it.

## TLS

Mbed TLS 3.6 (TLS 1.2 and 1.3, X.509 certificates, cryptography): link with
`-lmbedtls -lmbedx509 -lmbedcrypto` (pkg-config `mbedtls`) and libgcc's
`$(cc -print-libgcc-file-name)`. The standard root certificates are in
`/etc/ssl/certs/ca-certificates.crt` on Vexa; randomness comes from `/dev/urandom`.
Vexa's `fetch` (`userland/fetch`) is an example: an HTTPS client in 300 lines.

## Licenses

libvexa is part of Vexa (see the repository). Programs built with the SDK carry libm
from musl (MIT, `licenses/musl-libm.txt`) in libvexa, and SDL (zlib,
`licenses/SDL2.txt`), SDL_mixer (zlib, `licenses/SDL2_mixer.txt`) and SDL_net (zlib,
`licenses/SDL2_net.txt`) when they use them, and Mbed TLS (Apache-2.0,
`licenses/mbedtls.txt`); Vexa's SDL drivers are under SDL's license.
