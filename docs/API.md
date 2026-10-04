# libvexa API reference

libvexa is Vexa's own C library: the C basics, and Vexa's interfaces under `<vexa/...>`.
Programs in `userland/` are built against it (see the
[developer guide](DEVELOPER-GUIDE.md)). The headers are in `libvexa/include`; the
system call numbers, structures and constants they use are in `abi/vexa/abi.h`.

Unless it says otherwise, a function returns 0 (or a count, a handle, a size) on
success and a **negative `VX_E*` error** on failure; `vx_strerror(error)` describes one.

- [The C basics](#the-c-basics)
- [`<vexa/syscall.h>`: system calls](#vexasyscallh-system-calls)
- [`<vexa/thread.h>`: threads](#vexathreadh-threads)
- [`<vexa/net.h>`: networking](#vexaneth-networking)
- [`<vexa/gui.h>`: drawing and windows](#vexaguih-drawing-and-windows)
- [`<vexa/app.h>`: app bundles](#vexaapph-app-bundles)
- [`<vexa/files.h>`: whole files and folders](#vexafilesh-whole-files-and-folders)
- [`<vexa/settings.h>`: settings files](#vexasettingsh-settings-files)
- [`<vexa/time.h>`: dates and time zones](#vexatimeh-dates-and-time-zones)
- [`<vexa/font.h>`: the bitmap font](#vexafonth-the-bitmap-font)
- [`<vexa/desktop.h>`: the desktop protocol](#vexadesktoph-the-desktop-protocol)
- [Constants and structures](#constants-and-structures)

## The C basics

libvexa has the C standard library and much of POSIX, so programs written for other
systems mostly build as they are (the [SDK](SDK.md) builds them on another machine).
POSIX functions follow POSIX: they return -1 and set `errno` (Linux's numbers) on
failure. The `vx_*` functions under `<vexa/...>` are Vexa's own.

| Header | What's there |
| --- | --- |
| `<stdio.h>` | `FILE` and the standard streams; `fopen`, `freopen`, `fdopen`, `fclose`, `fflush`, `setvbuf`; `fread`, `fwrite`, `fgetc`, `fgets`, `getline`, `getdelim`, `ungetc`, `fputc`, `fputs`, `puts`; `fseek`, `ftell`, `rewind`, `fgetpos`, `fsetpos`; `printf` and friends (with `%f %e %g %a`, exactly rounded), `asprintf`, `dprintf`; `scanf`, `fscanf`, `sscanf`; `tmpfile`, `popen`, `pclose`, `perror`, `remove`, `rename`; memory as a file: `open_memstream`, `fmemopen`, `fopencookie` |
| `<stdlib.h>` | `malloc`, `calloc`, `realloc`, `free`, `aligned_alloc`, `posix_memalign`; `getenv`, `setenv`, `unsetenv`, `putenv`, `environ`; `strtol` and the rest, `strtod`, `strtof`, `strtold`, `atof`; `qsort`, `bsearch`; `rand`, `random`; `abs`, `div`; `exit`, `atexit`, `_Exit`, `system`; `mkstemp`, `mkdtemp`, `realpath`; `mbtowc` and the UTF-8 multibyte functions |
| `<string.h>`, `<strings.h>` | the `mem*` and `str*` functions, `strtok_r`, `strsep`, `strlcpy`, `strlcat`, `stpcpy`, `strcasecmp`, `strcasestr`, `strerror`, `strsignal` |
| `<ctype.h>`, `<wchar.h>` | character classes; wide strings (`wchar_t` is a code point) and UTF-8 conversions, `wcwidth` |
| `<math.h>` | all of C's math library: musl's libm |
| `<time.h>`, `<sys/time.h>` | `time`, `clock_gettime`, `nanosleep`, `gettimeofday`, `gmtime`, `localtime` (the time zone from Settings), `mktime`, `timegm`, `strftime` |
| `<unistd.h>`, `<fcntl.h>`, `<sys/stat.h>`, `<dirent.h>` | file descriptors: `open`, `read`, `write`, `pread`, `lseek`, `close`, `ftruncate`, `fsync`, `pipe`; `stat`, `fstat`, `lstat`, `mkdir`, `rmdir`, `unlink`, `rename`, `access`, `chdir`, `getcwd`, `symlink`, `readlink`, `isatty`; `opendir`, `readdir`, `closedir`; `sleep`, `usleep`, `getpid`, `sysconf`, `getopt` |
| `<pthread.h>`, `<semaphore.h>`, `<sched.h>` | threads, mutexes (normal, recursive, error-checking), condition variables, read-write locks, spin locks, barriers, `pthread_once`, keys; semaphores; `sched_yield`. Thread-local variables (`__thread`, `_Thread_local`, C++'s `thread_local`) work in programs and in libraries, also ones loaded with `dlopen` |
| `<sys/mman.h>`, `<poll.h>` | `mmap` (anonymous, and of files), `munmap`, `mprotect`; `poll` |
| `<signal.h>`, `<setjmp.h>` | `signal`, `sigaction` (handlers run for `raise`; from outside, signals can be ignored or end the program), `raise`, `kill`; `setjmp`, `longjmp` |
| `<errno.h>`, `<assert.h>`, `<limits.h>`, `<inttypes.h>`, `<locale.h>` | `errno` (one per thread), `assert`, limits, `PRId64` and friends, the "C" locale |
| `<endian.h>`, `<alloca.h>`, `<syslog.h>`, `<sys/file.h>` | byte order (`htobe32` and friends); `alloca`; `syslog` (to standard error: there's no system log); `flock` (accepted, but nothing is locked) |

The compiler's own headers work too: `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`,
`<stdarg.h>`, `<float.h>`. `main(int argc, char **argv)` is called as usual; returning
from it ends the program with that exit code.

Also there: `dup` and `dup2`, `posix_spawn` (`<spawn.h>`), and `dlopen`, `dlsym` and
`dlclose` (`<dlfcn.h>`, for libraries in `/lib`). Not there: `fork` and `exec*`
(`vx_spawn` and `posix_spawn` start programs), locales other than "C", and user and
group IDs beyond stubs.

## `<vexa/syscall.h>`: system calls

### The program

| Function | What it does |
| --- | --- |
| `void vx_exit(int code)` | ends the program |
| `long vx_process_id(void)` | this process's id |
| `long vx_yield(void)` | lets other threads run |
| `long vx_sleep(uint64_t ms)` | sleeps |
| `long vx_uptime(void)` | milliseconds since boot |
| `long vx_time(void)` | seconds since 1970-01-01 UTC (0 if the clock is unknown) |
| `long vx_log(const char *text, size_t length)` | writes to the kernel log |
| `long vx_system_info(struct vx_system_info *info)` | version, CPUs, memory, uptime |
| `long vx_kernel_command(const char *command)` | runs a kernel monitor command (`"disks"`, `"threads"`...), printing to the console |
| `long vx_power(int action)` | `VX_POWER_OFF` (ACPI) or `VX_POWER_RESTART`; doesn't return |
| `long vx_mounts(struct vx_mount_info *mounts, size_t count)` | the mounted file systems (path, source, type, total and free bytes, read-only); returns how many there are |
| `long vx_get_hostname(char *buffer, size_t size)`, `long vx_set_hostname(const char *name)` | the computer's name (letters, digits, `-`, `.`) |
| `const char *vx_strerror(long error)` | describes a negative `VX_E*` |

### Files

Paths are C strings; relative ones start from the current folder.

| Function | What it does |
| --- | --- |
| `int vx_open(const char *path, unsigned flags)` | opens a file, folder or device; returns a handle. Flags: `VX_OPEN_READ`, `WRITE`, `CREATE`, `TRUNCATE`, `APPEND`, `NO_FOLLOW` |
| `long vx_close(int handle)` | |
| `long vx_read(int handle, void *buffer, size_t size)` | returns how many bytes (0 at the end) |
| `long vx_write(int handle, const void *buffer, size_t size)` | returns how many bytes |
| `long vx_seek(int handle, long offset, int whence)` | `VX_SEEK_SET`, `CURRENT`, `END`; returns the new position |
| `long vx_stat(const char *path, struct vx_stat *stat)` | size, type, links, modified time, mode (follows links) |
| `long vx_lstat(const char *path, struct vx_stat *stat)` | the same, for a link itself |
| `long vx_handle_stat(int handle, struct vx_stat *stat)` | the same, for an open handle |
| `long vx_read_dir(int handle, struct vx_dir_entry *entries, size_t count)` | the next entries of an open folder (name, type, inode); 0 at the end |
| `long vx_mkdir(const char *path)` | makes a folder |
| `long vx_remove(const char *path)` | removes a file, link or empty folder |
| `long vx_rename(const char *from, const char *to)` | renames or moves on one file system (`-VX_EXDEV` across them; `vx_move` copies) |
| `long vx_symlink(const char *target, const char *path)` | makes a symbolic link |
| `long vx_readlink(const char *path, char *buffer, size_t size)` | where a link points (not NUL-terminated; returns the length) |
| `long vx_chdir(const char *path)`, `long vx_getcwd(char *buffer, size_t size)` | the current folder |
| `long vx_pipe(int handles[2])` | a pipe: `[0]` reads, `[1]` writes |
| `long vx_resize(int handle, unsigned long size)` | a file's new length (zero-filled) |

### Processes

| Function | What it does |
| --- | --- |
| `int vx_spawn(const char *path, const struct vx_spawn *spawn)` | starts a program; returns a **process handle**. `struct vx_spawn`: `argv`/`argc`, `envp`/`envc`, `handles[3]` (the child's 0, 1, 2; -1 for none), `flags` (`VX_SPAWN_NEW_GROUP`, `VX_SPAWN_JOIN_GROUP` with `group`) |
| `long vx_wait(int process, unsigned flags)` | waits for it to end; returns its exit code (`VX_WAIT_NO_HANG`: `-VX_EAGAIN` if it hasn't) |
| `long vx_handle_process_id(int process)` | the process id behind a handle |
| `long vx_kill(long process_id, int signal)` | sends a signal to a process |
| `long vx_priority(long process_id, int nice, int *now)` | sets a process's nice value (-20 runs first, 19 last; `VX_PRIORITY_GET` just reads it); children inherit it |
| `long vx_signal(int signal, int action)` | `VX_SIGNAL_DEFAULT` or `VX_SIGNAL_IGNORE` (Vexa programs can't catch signals) |
| `long vx_set_foreground(long group)` | which process group the terminal's Ctrl-C goes to |
| `long vx_process_list(struct vx_process_info *entries, size_t count)` | the processes (id, parent, group, state, memory, name) |

```c
const char *argv[] = {"/bin/ls", "-l", "/tmp"};
struct vx_spawn spawn = {.argv = argv, .argc = 3, .envp = (const char *const *)environ,
                         .envc = 0 /* count them */, .handles = {0, 1, 2}};
int child = vx_spawn(argv[0], &spawn);
long code = child >= 0 ? vx_wait(child, 0) : child;
```

### Memory

| Function | What it does |
| --- | --- |
| `void *vx_map(size_t size, unsigned flags)` | zeroed memory (`VX_MAP_WRITE`, `VX_MAP_EXEC`); NULL if out of memory |
| `long vx_unmap(void *address, size_t size)` | gives it back |
| `long vx_protect(void *address, size_t size, unsigned flags)` | changes access (page aligned) |
| `void *vx_map_file(int handle, unsigned long offset, size_t size, unsigned flags)` | maps a file or device, **shared**: writes reach the file and every other mapping |

### Threads and waiting (low level)

| Function | What it does |
| --- | --- |
| `long vx_thread_start(const struct vx_thread_start *start)` | starts a thread (use `<vexa/thread.h>`) |
| `void vx_thread_exit(int code)`, `long vx_thread_id(void)` | |
| `long vx_wait_address(volatile unsigned *address, unsigned expected, long timeout_ms)` | sleeps while `*address == expected` until woken (0), timed out (`-VX_ETIMEDOUT`; -1 waits forever), or `-VX_EAGAIN` if it wasn't `expected` |
| `long vx_wake_address(volatile unsigned *address, long count)` | wakes up to `count` waiters; returns how many |

### Sockets

`vx_read` and `vx_write` work on connected sockets too.

| Function | What it does |
| --- | --- |
| `int vx_socket(int family, int type, int protocol)` | `VX_AF_INET`, `VX_AF_INET6` (which takes IPv4 too, as `::ffff:a.b.c.d`) or `VX_AF_UNIX`; `VX_SOCK_STREAM` or `VX_SOCK_DGRAM` (or'd with `VX_SOCK_NONBLOCK`) |
| `long vx_bind(int h, const struct vx_socket_address *a, size_t length)` | a port, or a path for a local socket |
| `long vx_listen(int h, int backlog)`, `int vx_accept(int h, struct vx_socket_address *peer, unsigned flags)` | a server |
| `long vx_connect(int h, const struct vx_socket_address *a, size_t length)` | a client |
| `long vx_send(int h, struct vx_message *m)`, `long vx_receive(int h, struct vx_message *m)` | with addresses (UDP) and passed handles (local sockets) |
| `long vx_shutdown(int h, int how)` | `VX_SHUT_READ`, `VX_SHUT_WRITE`, `VX_SHUT_BOTH` |
| `long vx_socket_address(int h, int peer, struct vx_socket_address *a)` | its own address (peer 0) or its peer's |
| `long vx_socket_pair(int family, int type, int handles[2])` | two connected local sockets |
| `long vx_poll(struct vx_poll *handles, size_t count, long timeout_ms)` | waits until one is ready (`VX_POLL_READ`, `WRITE`; `ERROR`, `HANGUP` always reported); returns how many |
| `long vx_net_info(struct vx_net_interface *interfaces, size_t count)` | the network interfaces |

### Devices

| Function | What it does |
| --- | --- |
| `long vx_control(int handle, unsigned request, void *arg, size_t size)` | a device request, such as `VX_INPUT_INFO`, `VX_INPUT_GRAB`, `VX_DISPLAY_INFO`, `VX_DISPLAY_ACQUIRE`, or a terminal's `VX_TTY_GET_SIZE`, `VX_TTY_SET_SIZE` and `VX_TTY_PTY_NUMBER` |
| `long vx_device_list(struct vx_device_info *devices, size_t count, unsigned long long *generation)` | the devices the kernel knows, parents before children: up to `count` of them; returns how many there are. `generation` changes whenever a device comes, goes or changes (pass `NULL, 0` to read only that) |

Each `struct vx_device_info` has an `id` and its `parent`'s (0 at the top), a `bus`
(`VX_BUS_PCI`, `VX_BUS_USB`, `VX_BUS_PLATFORM`, `VX_BUS_VIRTUAL` for what drivers make,
like a disk on a controller), a `kind` (`VX_DEVICE_DISK`, `VX_DEVICE_KEYBOARD`,
`VX_DEVICE_USB_HUB`...), PCI or USB `vendor_id` and `product_id`, `flags`
(`VX_DEVICE_HAS_DRIVER`, `VX_DEVICE_REMOVABLE`), and text: `name`, `driver`, `location`
(`"PCI 00:1f.3"`, `"USB port 2"`, `"/dev/usb0"`) and `details`.

Input devices (`/dev/input/eventN`) give `struct vx_input_event` records (type, code,
value, time) when read. event0 is every keyboard and event1 every pointer (mice,
touchpads, tablets), whatever is plugged in, so most programs want those two; the
others are the devices one by one. Mice report `VX_EV_REL` motion (and
`VX_REL_WHEEL`, `VX_REL_HWHEEL`); tablets report `VX_EV_ABS` positions, `VX_ABS_X` and
`VX_ABS_Y` from 0 to `VX_ABS_MAX` across the screen (`VX_INPUT_ABSOLUTE` in
`VX_INPUT_INFO`'s capabilities). A device that's unplugged sends nothing more;
`/dev/display0` is the screen (mapped with `vx_map_file` after `VX_DISPLAY_ACQUIRE`).
Reading or writing a disk that was unplugged gives `-VX_ENODEV` or `-VX_EIO`.

`/dev/audio0` plays sound: write 16-bit little-endian samples (interleaved, when
stereo). `VX_AUDIO_SET_FORMAT` (`struct vx_audio_format`: 44100 or 48000 Hz, 1 or 2
channels) comes first; a write waits while the buffer is full; `VX_AUDIO_DELAY` says how
many frames are still to play, `VX_AUDIO_DRAIN` waits for them, `VX_AUDIO_DROP` throws
them away, and closing the handle lets them finish. `VX_AUDIO_INFO` describes the device.

## `<vexa/thread.h>`: threads

```c
static struct vx_mutex lock = VX_MUTEX_INIT;

static void *work(void *arg) {
    vx_mutex_lock(&lock);
    /* ... */
    vx_mutex_unlock(&lock);
    return arg;
}

struct vx_thread *t = vx_thread_create(work, NULL);
void *result = vx_thread_join(t);
```

| Function | What it does |
| --- | --- |
| `struct vx_thread *vx_thread_create(void *(*fn)(void *), void *arg)` | runs `fn(arg)` in a new thread (256 KiB stack); NULL on failure |
| `void *vx_thread_join(struct vx_thread *thread)` | waits for it, frees it, returns what `fn` returned |
| `void vx_mutex_lock(struct vx_mutex *m)`, `void vx_mutex_unlock(struct vx_mutex *m)` | a lock (`VX_MUTEX_INIT`); waiting threads sleep |

## `<vexa/net.h>`: networking

| Function | What it does |
| --- | --- |
| `int vx_connect_to(const char *host, uint16_t port)` | resolves `host` and opens a TCP connection (by IPv4, else IPv6); returns the socket |
| `long vx_resolve(const char *name, uint32_t *address)` | a numeric address, `localhost`, `/etc/hosts`, or DNS; `-VX_ENOENT` if there's no such name |
| `long vx_resolve6(const char *name, uint8_t address[16])` | the same for an IPv6 address (DNS AAAA records) |
| `struct vx_socket_address vx_inet6_address(const uint8_t address[16], uint16_t port)` | an IPv6 socket address (port in host order) |
| `int vx_parse_ipv6(const char *text, uint8_t address[16])`, `char *vx_format_ipv6(const uint8_t address[16], char text[46])` | "2001:db8::1" and back |
| `struct vx_socket_address vx_inet_address(uint32_t address, uint16_t port)` | an IPv4 socket address (address in network order, port in host order) |
| `int vx_parse_ipv4(const char *text, uint32_t *address)`, `char *vx_format_ipv4(uint32_t address, char text[16])` | "10.0.2.2" and back |
| `uint16_t vx_net16(uint16_t value)` | swaps bytes (host and network order) |

```c
int s = vx_connect_to("example.com", 80);
const char *request = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
vx_write(s, request, strlen(request));
char buffer[4096];
long n;
while ((n = vx_read(s, buffer, sizeof(buffer))) > 0) {
    fwrite(buffer, 1, (size_t)n, stdout);
}
vx_close(s);
```

## `<vexa/gui.h>`: drawing and windows

Colors are `0xRRGGBB`. `VX_TRANSPARENT` as a background leaves what's there.

### Drawing

| Function | What it does |
| --- | --- |
| `struct vx_surface { uint32_t *pixels; int width, height, stride; }` | pixels in memory |
| `void vx_fill(s, x, y, width, height, color)` | a rectangle (clipped) |
| `void vx_blit(to, tx, ty, from, fx, fy, width, height)` | copies a rectangle between surfaces |

### Text

Text is UTF-8, drawn smooth with TrueType fonts: DejaVu Sans, Sans Bold and Sans Mono
in `/share/fonts`, rasterized by [stb_truetype](https://github.com/nothings/stb) (in
`third_party/stb`). A font is a face at a size in pixels; each program keeps the glyphs
it has drawn. Without the font files, text falls back to the console's 8x16 bitmap font.

```c
const struct vx_font *title = vx_font(VX_FACE_BOLD, 24);
int end = vx_text(s, title, 20, 10, "Größe", VX_COLOR_TEXT, VX_TRANSPARENT);
vx_draw_text(s, end + 8, 18, "café", VX_COLOR_DIM, VX_TRANSPARENT); /* The UI font. */
```

| Function | What it does |
| --- | --- |
| `const struct vx_font *vx_font(int face, int size)` | `VX_FACE_SANS`, `VX_FACE_BOLD` or `VX_FACE_MONO` at `size` pixels (the em) |
| `const struct vx_font *vx_font_ui(void)` | the apps' text: Sans at `VX_UI_FONT_SIZE` (13) |
| `int vx_font_height(font)`, `int vx_font_ascent(font)` | a line's height; the baseline's distance from its top |
| `int vx_text(s, font, x, y, text, fg, bg)` | a line of text with its top at `y`; returns the x after it (`bg`: filled behind it, unless `VX_TRANSPARENT`) |
| `int vx_text_width_font(font, text)`, `int vx_text_width(text)` | how wide text is (the second: in the UI font) |
| `int vx_text_width_bytes(font, text, length)` | the width of its first `length` bytes |
| `size_t vx_text_fit_bytes(font, text, width)` | how many bytes of it fit in `width` pixels (whole characters) |
| `int vx_draw_text(s, x, y, text, fg, bg)` | UI-font text, centered in a `VX_LINE_HEIGHT` (16) line whose top is `y` |
| `void vx_draw_char(s, x, y, uint32_t c, fg, bg)` | one character in a monospaced cell, `VX_CELL_WIDTH` (8) by `VX_LINE_HEIGHT`: terminals and editors |
| `int vx_font_cell_width(font)` | a monospaced font's cell width (its widest advance) |
| `void vx_draw_cell(s, font, x, y, width, height, uint32_t c, fg, bg)` | one character in a `width` by `height` cell of any font and size: the Terminal's cells (box-drawing and block characters are drawn to fill the cell) |
| `uint32_t vx_utf8_next(const char **text)` | the character at `*text`, moving past it (0xFFFD for a bad byte) |
| `int vx_utf8_encode(uint32_t c, char out[4])` | a character's bytes; returns how many |
| `size_t vx_utf8_previous(const char *text, size_t at)` | where the character before byte `at` starts |

### Windows

| Function | What it does |
| --- | --- |
| `struct vx_window *vx_window_create(const char *title, int width, int height)` | a window (NULL if no desktop is running); draw into `window->surface` |
| `struct vx_window *vx_window_create_flags(title, width, height, unsigned flags)` | `VX_WINDOW_RESIZABLE`: the user can resize it (you get `VX_GUI_RESIZE`) |
| `int vx_window_resize(struct vx_window *w, int width, int height)` | a new, blank surface of that size |
| `void vx_window_present(struct vx_window *w, int x, int y, int width, int height)` | shows what was drawn in the rectangle |
| `void vx_window_set_title(w, const char *title)`, `void vx_window_destroy(w)` | |
| `void vx_window_set_cursor(w, int shape)` | the pointer's shape over the window: `VX_CURSOR_ARROW`, `TEXT`, `HAND`, `WAIT`, `CROSS`, `MOVE` (sent only when it changes) |
| `void vx_window_drag_files(w, const char *const *paths, int count, bool copy)` | files dragged out of the window and let go where the pointer is now: the desktop puts them there (on the desktop, or in another window as a `VX_GUI_DROP`) |
| `char *vx_drop_paths(const struct vx_gui_event *e)` | a `VX_GUI_DROP`'s paths, one per line, in a string to `free()` |
| `int vx_gui_wait(struct vx_gui_event *e, long timeout_ms)` | the next event: 1, 0 on timeout (-1 waits forever), `-VX_EPIPE` if the desktop is gone |
| `int vx_gui_handle(void)` | the desktop connection's handle, for `vx_poll` |
| `void vx_notify(const char *text)` | a notification on the desktop ("title: text") |
| `void vx_desktop_reload(void)` | asks the desktop to read `/etc/desktop.conf` again |
| `void vx_desktop_lock(void)` | locks the screen |
| `void vx_clipboard_set(const char *text, size_t length)` | puts text on the clipboard every app shares (X programs too) |
| `char *vx_clipboard_get(void)` | the clipboard's text, to `free()` (NULL if it's empty) |
| `bool vx_open_dialog(title, folder, char *out, size_t size)` | the Open dialog (starting in `folder`, or home): the chosen file's path in `out`; false if cancelled |
| `bool vx_save_dialog(title, folder, name, char *out, size_t size)` | the Save dialog, with `name` filled in: the path to save to in `out`; false if cancelled |

`struct vx_gui_event` fields, by type:

| `type` | Fields |
| --- | --- |
| `VX_GUI_KEY` | `key` (`VX_KEY_*`, Linux key codes), `value` (1 down, 0 up, 2 repeat), `character` (what it types, in Unicode, in the keyboard layout Settings chose; or 0; Ctrl+letter gives 1-26) |
| `VX_GUI_POINTER` | `x`, `y` (in the window), `buttons` (bit 0 left, 1 right, 2 middle), `wheel`. While a button pressed in the window is down, the window gets the pointer even outside it (`x`, `y` beyond its size) |
| `VX_GUI_DROP` | `x`, `y`, `value` (1 to copy, not move), `text`: files dropped on the window (`vx_drop_paths`) |
| `VX_GUI_CLOSE` | the user asked to close the window |
| `VX_GUI_FOCUS` | `value`: 1 gained the keyboard, 0 lost it |
| `VX_GUI_RESIZE` | `width`, `height`: the size the user asked for |
| `VX_GUI_THEME` | the theme (or another setting) changed: `vx_theme` has the new one; draw again |

### Widgets

The look of Vexa's own apps: `VX_COLOR_WINDOW`, `VIEW`, `TEXT`, `DIM`, `ACCENT`,
`SELECTED`, `BUTTON`, `BUTTON_HOT`, `LINE`. They come from the theme, `vx_theme` (a
`struct vx_theme`: `dark`, and those colors plus `sidebar`, `stripe`, `shadow` and the
desktop's), which libvexa reads from `/etc/desktop.conf` when a window opens and again
when it changes. `vx_theme_make(&theme, "light", "blue")` makes one (for a preview);
`vx_accents[]` (`vx_accent_count`) are the accent colors; `vx_mix(a, b, amount)` mixes
two colors.

| Function | What it does |
| --- | --- |
| `void vx_draw_outline(s, x, y, width, height, color)` | a one-pixel outline |
| `void vx_draw_text_fit(s, x, y, width, text, fg, bg)` | text cut to fit, ending in "…" |
| `void vx_draw_button(s, x, y, width, height, label, bool hot)` | a push button |
| `void vx_draw_field(s, x, y, width, text, bool focused)` | a one-line text field |
| `bool vx_field_key(char *text, size_t size, const struct vx_gui_event *e)` | edits a field's text (UTF-8) with a key event; true if it changed |
| `bool vx_inside(px, py, x, y, width, height)` | a point in a rectangle |
| `void vx_menu_size(items, count, int *width, int *height)` | a pop-up menu's size |
| `void vx_draw_menu(s, x, y, items, count, int hot)` | draws it (`hot`: the item under the pointer, or -1) |
| `int vx_menu_item_at(items, count, x, y, px, py)` | the item at a point, or -1 |

A menu is an array of `struct vx_menu_item { const char *label; const char *keys;
bool disabled; }`; an item with no label is a line between groups.

### Images

| Function | What it does |
| --- | --- |
| `struct vx_image *vx_image_load(const char *path, uint32_t background)` | reads a PNG (8 bits a channel, not interlaced), BMP (24, 32 bits) or PPM (P6); transparency is blended onto `background`, or kept with `VX_IMAGE_ALPHA` (pixels are then `0xAARRGGBB`); NULL on failure |
| `struct vx_image *vx_image_decode(const void *data, size_t size, uint32_t background)` | the same, from memory |
| `void vx_image_free(struct vx_image *image)` | |
| `void vx_blit_scaled(to, x, y, width, height, from)` | draws a surface scaled (nearest pixel) |
| `void vx_blit_alpha(to, x, y, width, height, from)` | draws a `VX_IMAGE_ALPHA` image scaled and blended (averaged when smaller: icons) |
| `int vx_image_save_png(const char *path, const struct vx_surface *s)` | writes a surface as a PNG (RGB; deflate-compressed): 0 or an error |

## `<vexa/app.h>`: app bundles

An app is a folder `Name.vxapp` in `/apps` (see the developer guide for its
`Contents/Info.conf`). `struct vx_app` has the bundle's path, `name`, `executable` and
`icon` (full paths), `opens`, `shortcut`, `menu`, `desktop` and `is_linux`.

| Function | What it does |
| --- | --- |
| `bool vx_app_is_bundle(const char *path)` | its name ends in `.vxapp` |
| `int vx_app_load(const char *bundle, struct vx_app *app)` | reads a bundle's Info.conf |
| `int vx_app_list(struct vx_app *apps, int max)` | the apps in `/apps` whose programs are there, by menu place, then name; returns how many |
| `int vx_app_find(const char *name, struct vx_app *app)` | an app by name ("Text Editor") or bundle name ("Editor", "Editor.vxapp"), or a path |
| `int vx_app_for_file(const char *path, struct vx_app *app)` | the app that opens a file: the first listing its extension, else the first with `opens=*` |
| `int vx_app_open(const struct vx_app *app, const char *file)` | starts it (with a file to open, or NULL); returns a process handle |

## `<vexa/files.h>`: whole files and folders

What a file manager needs; each stops at the first error.

| Function | What it does |
| --- | --- |
| `long vx_copy_tree(const char *from, const char *to)` | copies a file, link, or folder with everything in it (`to` must not exist) |
| `long vx_remove_tree(const char *path)` | removes a file, link, or folder with everything in it |
| `long vx_move(const char *from, const char *to)` | renames; across file systems, copies then removes |
| `unsigned long long vx_tree_size(const char *path, long *files)` | a file's size, or the total of a folder's files (counting them) |
| `void vx_unique_name(const char *dir, const char *name, char *out, size_t size)` | a name not taken in `dir`: `name`, else "stem 2.ext", "stem 3.ext"... |
| `void vx_join_path(char *out, size_t size, const char *dir, const char *name)` | "dir/name" with one slash |

## `<vexa/settings.h>`: settings files

`key=value` files in `/etc`, kept on disk too when there is one (see the architecture).

```c
struct vx_settings s;
vx_settings_load(&s, "desktop.conf");
int hours = vx_settings_int(&s, "clock", 24);
vx_settings_set(&s, "clock", "12");
vx_settings_save(&s);
vx_desktop_reload(); /* The desktop, and every program, read them again. */
```

| Function | What it does |
| --- | --- |
| `int vx_settings_load(struct vx_settings *s, const char *name)` | reads `/etc/<name>` (none yet: empty) |
| `const char *vx_settings_get(s, key, fallback)`, `int vx_settings_int(...)`, `bool vx_settings_bool(...)` | a value, or the fallback (booleans: yes/no, 1/0, true/false, on/off) |
| `vx_settings_set`, `vx_settings_set_int`, `vx_settings_set_bool`, `vx_settings_unset` | change one |
| `int vx_settings_save(const struct vx_settings *s)` | writes `/etc/<name>`, and the copy on disk |
| `int vx_settings_write_file(const char *name, const char *text, size_t length)` | a whole file in `/etc` (and on disk) |
| `bool vx_settings_disk(char *out, size_t size)` | the disk that keeps settings, if any |
| `int vx_settings_restore(void)` | copies them back to `/etc` (vinit does it at boot) |
| `void vx_password_hash(const char *password, char out[17])` | the lock screen password's hash, as `lock_password` keeps it |

## `<vexa/time.h>`: dates and time zones

| Function | What it does |
| --- | --- |
| `void vx_date_of(long seconds, struct vx_date *d)`, `long vx_seconds_of(const struct vx_date *d)` | seconds since 1970 to a date (year, month, day, hour, minute, second, weekday), and back |
| `const struct vx_zone *vx_find_zone(const char *city)` | a zone from `vx_zones[]` (`vx_zone_count`): a city, its region, its standard offset and its summer time rule |
| `int vx_zone_offset(const struct vx_zone *zone, long utc_seconds)` | its offset from UTC at that moment, in minutes |
| `void vx_local_now(struct vx_date *d)` | the local time now, in the zone Settings chose |

`vx_month_names[12]` and `vx_weekday_names[7]` are the names.

## `<vexa/font.h>`: the bitmap font

The console's font (Spleen 8x16): `FONT_WIDTH` 8, `FONT_HEIGHT` 16, printable ASCII
from `FONT_FIRST_CHAR` (0x20), `FONT_GLYPH_COUNT` glyphs in `font_glyphs`, one byte a
row, most significant bit on the left. Text falls back to it when the TrueType fonts
aren't there.

## `<vexa/desktop.h>`: the desktop protocol

The messages between programs and the desktop over `/run/desktop` (`DESKTOP_SOCKET`);
`<vexa/gui.h>` wraps them, and the developer guide describes them. It also has the
settings file's name (`DESKTOP_CONFIG`, `/etc/desktop.conf`), the default wallpaper
(`DESKTOP_DEFAULT_WALLPAPER`) and the gradients Settings offers
(`desktop_wallpapers[]`).

## Constants and structures

From `abi/vexa/abi.h` (included by `<vexa/syscall.h>`):

| Name | Values |
| --- | --- |
| Errors (`VX_E*`) | `NOSYS`, `FAULT`, `INVAL`, `NOENT`, `EXIST`, `NOTDIR`, `ISDIR`, `NOTEMPTY`, `BADF`, `ACCES`, `NOSPC`, `IO`, `NAMETOOLONG`, `MFILE`, `NOMEM`, `ROFS`, `BUSY`, `XDEV`, `INTR`, `PIPE`, `CHILD`, `SRCH`, `AGAIN`, `NOEXEC`, `2BIG`, `NOTTY`, `SPIPE`, `LOOP`, `TIMEDOUT`, and the socket errors (`CONNREFUSED`, `ADDRINUSE`, `NOTCONN`...) |
| File types (`VX_TYPE_*`) | `FILE`, `DIRECTORY`, `CHAR_DEVICE`, `BLOCK_DEVICE`, `SYMLINK`, `SOCKET` |
| Open flags (`VX_OPEN_*`) | `READ`, `WRITE`, `CREATE`, `TRUNCATE`, `APPEND`, `NO_FOLLOW` |
| Signals (`VX_SIG*`) | the usual Unix numbers: `HUP` 1, `INT` 2, `QUIT` 3, `KILL` 9, `TERM` 15, ... |
| Keys (`VX_KEY_*`) | Linux key codes: `ESC`, `BACKSPACE`, `TAB`, `ENTER`, `LEFTCTRL`, `LEFTSHIFT`, `LEFTALT`, `SPACE`, `F1`..., `HOME`, `UP`, `PAGEUP`, `LEFT`, `RIGHT`, `END`, `DOWN`, `PAGEDOWN`, `INSERT`, `DELETE`; letters and digits use their Linux codes (A is 30, C is 46, 1 is 2) |

`struct vx_stat`: `size`, `inode`, `type`, `links`, `modified` (seconds since 1970, or
0), `mode` (Unix permission bits). `struct vx_dir_entry`: `inode`, `type`, `name`.
`struct vx_system_info`: `version`, `cpus`, `memory_total`, `memory_free`, `uptime_ms`.
`struct vx_process_info`: `id`, `parent`, `group`, `state`, `memory`, `name`.
