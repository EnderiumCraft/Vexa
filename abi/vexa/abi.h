#ifndef VEXA_ABI_H
#define VEXA_ABI_H

/*
 * The native Vexa system call interface, shared by the kernel and libvexa.
 *
 * Calling convention (x86_64): the `syscall` instruction with the call number
 * in rax and up to six arguments in rdi, rsi, rdx, r10, r8, r9. The result
 * comes back in rax; a negative value is an error (-VX_E*). rcx and r11 are
 * clobbered, as the instruction requires.
 *
 * This interface may change freely until Vexa 1.0.
 */

#define VX_SYS_EXIT 0      /* vx_exit(int code): ends the process. */
#define VX_SYS_LOG 1       /* vx_log(const char *text, size_t length): prints text. */
#define VX_SYS_YIELD 2     /* vx_yield(): lets other threads run. */
#define VX_SYS_SLEEP 3     /* vx_sleep(uint64_t ms) */
#define VX_SYS_PROCESS_ID 4 /* vx_process_id() */
#define VX_SYS_UPTIME 5    /* vx_uptime(): milliseconds since boot. */

/* Files. Paths are passed as (pointer, length), not NUL-terminated. Paths
 * that don't start with '/' are relative to the root for now. */
#define VX_SYS_OPEN 6      /* vx_open(path, length, flags) -> handle */
#define VX_SYS_CLOSE 7     /* vx_close(handle) */
#define VX_SYS_READ 8      /* vx_read(handle, buffer, size) -> bytes read, 0 at the end */
#define VX_SYS_WRITE 9     /* vx_write(handle, buffer, size) -> bytes written */
#define VX_SYS_SEEK 10     /* vx_seek(handle, offset, whence) -> new position */
#define VX_SYS_STAT 11     /* vx_stat(path, length, struct vx_stat *) */
#define VX_SYS_READ_DIR 12 /* vx_read_dir(handle, struct vx_dir_entry *, count) -> entries */
#define VX_SYS_MKDIR 13    /* vx_mkdir(path, length) */
#define VX_SYS_REMOVE 14   /* vx_remove(path, length): a file or an empty directory */
#define VX_SYS_HANDLE_STAT 15 /* vx_handle_stat(handle, struct vx_stat *) */

/* Processes. A new process gets its arguments, environment and standard
 * handles (0 input, 1 output, 2 errors) from vx_spawn. */
#define VX_SYS_SPAWN 16    /* vx_spawn(path, length, struct vx_spawn *) -> process handle */
#define VX_SYS_WAIT 17     /* vx_wait(process handle, VX_WAIT_* flags) -> exit code */
#define VX_SYS_MAP 18      /* vx_map(size, VX_MAP_* flags) -> address of zeroed memory */
#define VX_SYS_UNMAP 19    /* vx_unmap(address, size) */
#define VX_SYS_PIPE 20     /* vx_pipe(int handles[2]): [0] reads, [1] writes */
#define VX_SYS_CHDIR 21    /* vx_chdir(path, length) */
#define VX_SYS_GETCWD 22   /* vx_getcwd(buffer, size) -> length */
#define VX_SYS_KILL 23     /* vx_kill(process id, signal) */
#define VX_SYS_SIGNAL 24   /* vx_signal(signal, VX_SIGNAL_DEFAULT or VX_SIGNAL_IGNORE) */
#define VX_SYS_SET_FOREGROUND 25 /* vx_set_foreground(process group): who gets Ctrl+C */
#define VX_SYS_SYSTEM_INFO 26    /* vx_system_info(struct vx_system_info *) */
#define VX_SYS_PROCESS_LIST 27   /* vx_process_list(struct vx_process_info *, count) -> count */
#define VX_SYS_KERNEL_COMMAND 28 /* vx_kernel_command(text, length): a kernel monitor command */
#define VX_SYS_CLOSE_ALL 29      /* reserved */
#define VX_SYS_RENAME 30   /* vx_rename(from, from_length, to, to_length) */
#define VX_SYS_HANDLE_PROCESS_ID 31 /* vx_handle_process_id(process handle) -> its id */
#define VX_SYS_SYMLINK 32  /* vx_symlink(target, target_length, path, path_length) */
#define VX_SYS_READLINK 33 /* vx_readlink(path, path_length, buffer, size) -> length */
#define VX_SYS_LSTAT 34    /* vx_lstat(path, length, struct vx_stat *): the link itself */
#define VX_SYS_THREAD_CREATE 35 /* vx_thread_create(const struct vx_thread_start *) -> thread id */
#define VX_SYS_THREAD_EXIT 36   /* vx_thread_exit(code): ends the calling thread only */
#define VX_SYS_THREAD_ID 37     /* vx_thread_id() -> the calling thread's id */
#define VX_SYS_WAIT_ADDRESS 38  /* vx_wait_address(address, expected, timeout_ms or -1) */
#define VX_SYS_WAKE_ADDRESS 39  /* vx_wake_address(address, count) -> how many woke */
#define VX_SYS_PROTECT 40       /* vx_protect(address, size, VX_MAP_* flags) */

/* Networking and local sockets (see "Sockets" below). */
#define VX_SYS_SOCKET 41   /* vx_socket(VX_AF_*, VX_SOCK_* type and flags, protocol) -> handle */
#define VX_SYS_BIND 42     /* vx_bind(handle, const struct vx_socket_address *, length) */
#define VX_SYS_LISTEN 43   /* vx_listen(handle, backlog) */
#define VX_SYS_ACCEPT 44   /* vx_accept(handle, struct vx_socket_address *peer or NULL,
                              VX_SOCK_NONBLOCK) -> handle */
#define VX_SYS_CONNECT 45  /* vx_connect(handle, const struct vx_socket_address *, length) */
#define VX_SYS_SEND 46     /* vx_send(handle, struct vx_message *) -> bytes sent */
#define VX_SYS_RECEIVE 47  /* vx_receive(handle, struct vx_message *) -> bytes received */
#define VX_SYS_SHUTDOWN 48 /* vx_shutdown(handle, VX_SHUT_*) */
#define VX_SYS_SOCKET_ADDRESS 49 /* vx_socket_address(handle, peer, struct vx_socket_address *) */
#define VX_SYS_SOCKET_PAIR 50    /* vx_socket_pair(VX_AF_UNIX, type, int handles[2]) */
#define VX_SYS_POLL 51     /* vx_poll(struct vx_poll *, count, timeout_ms or -1) -> ready count */
#define VX_SYS_NET_INFO 52 /* vx_net_info(struct vx_net_interface *, count) -> interfaces */

/* Devices. */
#define VX_SYS_CONTROL 53  /* vx_control(handle, request, void *arg, size): a device request */
#define VX_SYS_MAP_FILE 54 /* vx_map_file(handle, offset, size, VX_MAP_* flags) -> address,
                              shared: writes reach the file (or device) and other mappings */
#define VX_SYS_RESIZE 55   /* vx_resize(handle, size): a file's new length (zero-filled) */

#define VX_MAP_WRITE 0x1
#define VX_MAP_EXEC 0x2

/* vx_spawn flags. */
#define VX_SPAWN_NEW_GROUP 0x1 /* Start a new process group (for a shell's jobs). */
#define VX_SPAWN_JOIN_GROUP 0x2 /* Join the process group in vx_spawn.group. */

/* vx_wait flags. */
#define VX_WAIT_NO_HANG 0x1 /* Return -VX_EAGAIN at once if it hasn't exited. */

struct vx_spawn {
    const char *const *argv; /* argc strings; argv[0] is usually the program name. */
    unsigned long argc;
    const char *const *envp; /* "NAME=value" strings. */
    unsigned long envc;
    int handles[3];          /* The new process's 0, 1, 2; -1 for none. */
    unsigned int flags;      /* VX_SPAWN_* */
    unsigned int group;      /* With VX_SPAWN_JOIN_GROUP. */
};

/* Signals: the usual Unix numbers. Native programs can only ignore them or
 * take the default action (which, for most, ends the process). */
#define VX_SIGHUP 1
#define VX_SIGINT 2  /* Ctrl+C */
#define VX_SIGQUIT 3
#define VX_SIGILL 4
#define VX_SIGTRAP 5
#define VX_SIGABRT 6
#define VX_SIGBUS 7
#define VX_SIGFPE 8
#define VX_SIGKILL 9 /* Can't be ignored. */
#define VX_SIGUSR1 10
#define VX_SIGSEGV 11
#define VX_SIGUSR2 12
#define VX_SIGPIPE 13
#define VX_SIGALRM 14
#define VX_SIGTERM 15
#define VX_SIGCHLD 17
#define VX_SIGCONT 18
#define VX_SIGSTOP 19
#define VX_SIGTSTP 20
#define VX_SIGTTIN 21
#define VX_SIGTTOU 22
#define VX_SIGURG 23
#define VX_SIGWINCH 28
#define VX_SIGNAL_COUNT 64

#define VX_SIGNAL_DEFAULT 0
#define VX_SIGNAL_IGNORE 1

struct vx_system_info {
    char version[16];
    unsigned int cpus;
    unsigned int reserved;
    unsigned long long memory_total; /* Bytes. */
    unsigned long long memory_free;
    unsigned long long uptime_ms;
};

struct vx_process_info {
    unsigned int id;
    unsigned int parent;  /* 0 if none. */
    unsigned int group;
    unsigned int state;   /* 0 running, 1 exited */
    unsigned long long memory; /* Bytes of memory in use. */
    char name[32];
};

/* ---- Sockets ----
 * The constants and address layouts are the same as Linux's (AF_INET,
 * SOCK_STREAM, struct sockaddr_in...), in network byte order. */
#define VX_AF_UNIX 1 /* Local sockets, named by a path. */
#define VX_AF_INET 2 /* IPv4 */

#define VX_SOCK_STREAM 1
#define VX_SOCK_DGRAM 2
#define VX_SOCK_RAW 3 /* VX_AF_INET with VX_IPPROTO_ICMP only. */
#define VX_SOCK_SEQPACKET 5
#define VX_SOCK_TYPE_MASK 0xf
#define VX_SOCK_NONBLOCK 0x800 /* Calls return -VX_EAGAIN instead of waiting. */
#define VX_SOCK_CLOEXEC 0x80000

#define VX_IPPROTO_ICMP 1
#define VX_IPPROTO_TCP 6
#define VX_IPPROTO_UDP 17

/* vx_message flags. */
#define VX_MSG_PEEK 0x2       /* Receive without taking the data. */
#define VX_MSG_TRUNC 0x20     /* Set on return: a datagram was longer than the buffer. */
#define VX_MSG_DONTWAIT 0x40  /* Don't wait, this once. */
#define VX_MSG_WAITALL 0x100  /* Wait until the buffer is full (streams). */
#define VX_MSG_NOSIGNAL 0x4000 /* No SIGPIPE when the other side is gone. */

#define VX_SHUT_READ 0
#define VX_SHUT_WRITE 1
#define VX_SHUT_BOTH 2

struct vx_inet_address {
    unsigned short family;  /* VX_AF_INET */
    unsigned short port;    /* Network byte order. */
    unsigned int address;   /* Network byte order: 10.0.2.15 is bytes 10, 0, 2, 15. */
    unsigned char zero[8];
};

struct vx_unix_address {
    unsigned short family;  /* VX_AF_UNIX */
    char path[108];         /* NUL-terminated; a leading NUL means an abstract name. */
};

struct vx_socket_address {
    union {
        unsigned short family;
        struct vx_inet_address inet;
        struct vx_unix_address local;
        unsigned char storage[128];
    };
};

struct vx_message {
    void *data;
    unsigned long size;
    unsigned int flags;          /* VX_MSG_*; vx_receive may add VX_MSG_TRUNC. */
    unsigned int address_length; /* vx_send: of *address; vx_receive: set to it. */
    struct vx_socket_address *address; /* The destination, or the sender; may be NULL. */
};

/* vx_poll: which handles are ready. `events` and `ready` use VX_POLL_*. */
#define VX_POLL_READ 0x1   /* Something to read (or the end). */
#define VX_POLL_WRITE 0x4  /* Writing wouldn't wait. */
#define VX_POLL_ERROR 0x8  /* Always reported. */
#define VX_POLL_HANGUP 0x10 /* The other end is gone. Always reported. */

struct vx_poll {
    int handle;
    unsigned short events;
    unsigned short ready;
};

#define VX_NET_UP 0x1
#define VX_NET_LOOPBACK 0x2
#define VX_NET_DHCP 0x4 /* Configured by DHCP. */

struct vx_net_interface {
    char name[8];
    unsigned char mac[6];
    unsigned short reserved;
    unsigned int flags; /* VX_NET_* */
    unsigned int mtu;
    unsigned int address, netmask, gateway, dns; /* Network byte order; 0 if none. */
    unsigned long long rx_packets, rx_bytes, tx_packets, tx_bytes, rx_dropped, tx_dropped;
};

/* ---- Input devices: /dev/input/event0, event1... ----
 * Reading one gives whole struct vx_input_events. Types and codes are
 * Linux's (<linux/input-event-codes.h>): keys are KEY_A = 30 and so on. */
#define VX_EV_SYN 0x00 /* The end of a group of events that belong together. */
#define VX_EV_KEY 0x01 /* A key or button: value 1 pressed, 0 released, 2 repeated. */
#define VX_EV_REL 0x02 /* Relative motion. */

#define VX_REL_X 0x00
#define VX_REL_Y 0x01 /* Positive is down. */
#define VX_REL_WHEEL 0x08 /* Positive is away from the user. */
#define VX_BTN_LEFT 0x110
#define VX_BTN_RIGHT 0x111
#define VX_BTN_MIDDLE 0x112

#define VX_KEY_ESC 1
#define VX_KEY_BACKSPACE 14
#define VX_KEY_TAB 15
#define VX_KEY_ENTER 28
#define VX_KEY_LEFTCTRL 29
#define VX_KEY_LEFTSHIFT 42
#define VX_KEY_RIGHTSHIFT 54
#define VX_KEY_LEFTALT 56
#define VX_KEY_SPACE 57
#define VX_KEY_CAPSLOCK 58
#define VX_KEY_F1 59 /* F1 to F10 are 59 to 68 */
#define VX_KEY_RIGHTCTRL 97
#define VX_KEY_RIGHTALT 100
#define VX_KEY_HOME 102
#define VX_KEY_UP 103
#define VX_KEY_PAGEUP 104
#define VX_KEY_LEFT 105
#define VX_KEY_RIGHT 106
#define VX_KEY_END 107
#define VX_KEY_DOWN 108
#define VX_KEY_PAGEDOWN 109
#define VX_KEY_INSERT 110
#define VX_KEY_DELETE 111
#define VX_KEY_LEFTMETA 125

struct vx_input_event {
    unsigned long long time_ms; /* vx_uptime() when it happened. */
    unsigned short type;        /* VX_EV_* */
    unsigned short code;        /* VX_KEY_*, VX_BTN_*, VX_REL_* */
    int value;
};

#define VX_INPUT_KEYS 0x1    /* A keyboard. */
#define VX_INPUT_POINTER 0x2 /* A mouse: buttons and relative motion. */

/* vx_control requests for input devices. */
#define VX_INPUT_INFO 0x4901 /* struct vx_input_info (out) */
#define VX_INPUT_GRAB 0x4902 /* int (in): 1 = only this handle gets the events (for a
                                keyboard, the terminal stops getting keys); 0 = release */

struct vx_input_info {
    char name[64];
    unsigned int capabilities; /* VX_INPUT_* */
    unsigned int reserved;
};

/* ---- The display: /dev/display0 ----
 * vx_control(VX_DISPLAY_INFO) describes it; VX_DISPLAY_ACQUIRE takes the
 * screen over from the text console (until the handle is closed), and
 * vx_map_file then maps the frame buffer: `pitch` bytes per row, 32-bit
 * pixels. */
#define VX_DISPLAY_INFO 0x4401    /* struct vx_display_info (out) */
#define VX_DISPLAY_ACQUIRE 0x4402 /* no argument: -VX_EBUSY if someone else has it */

struct vx_display_info {
    unsigned int width, height; /* Pixels. */
    unsigned int pitch;         /* Bytes from one row to the next. */
    unsigned int bits_per_pixel;
    unsigned char red_shift, green_shift, blue_shift, reserved;
    unsigned int size;          /* Bytes to map. */
};

/* ---- Terminals ----
 * /dev/ptmx opens the controlling side of a new pseudo-terminal (what a
 * terminal window holds: it reads the programs' output and writes the keys
 * typed); /dev/pts/N is the terminal the programs in it see. */
#define VX_TTY_PTY_NUMBER 0x5401   /* int (out): N, on a /dev/ptmx handle */
#define VX_TTY_SET_SIZE 0x5402     /* struct vx_tty_size (in) */
#define VX_TTY_GET_SIZE 0x5403     /* struct vx_tty_size (out) */

struct vx_tty_size {
    unsigned short rows, columns;
};

/* vx_open flags. A handle can only be used the ways it was opened for. */
#define VX_OPEN_READ 0x1
#define VX_OPEN_WRITE 0x2
#define VX_OPEN_CREATE 0x4   /* Create the file if it doesn't exist. */
#define VX_OPEN_TRUNCATE 0x8 /* Empty the file first. */
#define VX_OPEN_APPEND 0x10  /* Every write goes to the end. */
#define VX_OPEN_NO_FOLLOW 0x20 /* Fail with VX_ELOOP if the path is a symbolic link. */

#define VX_SEEK_SET 0
#define VX_SEEK_CURRENT 1
#define VX_SEEK_END 2

#define VX_TYPE_FILE 1
#define VX_TYPE_DIRECTORY 2
#define VX_TYPE_CHAR_DEVICE 3
#define VX_TYPE_BLOCK_DEVICE 4
#define VX_TYPE_SYMLINK 5

#define VX_NAME_MAX 255
#define VX_PATH_MAX 4096

/* How a new thread starts: at `entry(arg)` on `stack` (rsp as given, so
 * pass a 16-byte aligned top minus 8, as if `entry` had been called), with
 * `tls` as its thread pointer (FS base). If `exit_word` isn't NULL, the
 * kernel stores 0 there and wakes one waiter on it when the thread ends,
 * which is how joining works. */
struct vx_thread_start {
    void *entry;
    void *stack;
    void *arg;
    void *tls;
    unsigned int *exit_word;
};

struct vx_stat {
    unsigned long long size;
    unsigned long long inode;
    unsigned int type; /* VX_TYPE_* */
    unsigned int links;
    long long modified; /* Seconds since 1970, or 0 if unknown. */
    unsigned int mode;  /* Permission bits, Unix style (e.g. 0755). */
    unsigned int reserved;
};

struct vx_dir_entry {
    unsigned long long inode;
    unsigned int type; /* VX_TYPE_* */
    unsigned int name_length;
    char name[VX_NAME_MAX + 1]; /* NUL-terminated. */
};

#define VX_ENOSYS 1        /* No such system call. */
#define VX_EFAULT 2        /* A pointer argument was not valid user memory. */
#define VX_EINVAL 3        /* An argument was out of range. */
#define VX_ENOENT 4        /* No such file or directory. */
#define VX_EEXIST 5        /* It already exists. */
#define VX_ENOTDIR 6       /* A path component is not a directory. */
#define VX_EISDIR 7        /* It is a directory. */
#define VX_ENOTEMPTY 8     /* The directory is not empty. */
#define VX_EBADF 9         /* Not an open handle. */
#define VX_EACCES 10       /* The handle wasn't opened for this. */
#define VX_ENOSPC 11       /* No space left on the device. */
#define VX_EIO 12          /* The device reported an error. */
#define VX_ENAMETOOLONG 13 /* A name or path is too long. */
#define VX_EMFILE 14       /* Too many open handles. */
#define VX_ENOMEM 15       /* Out of memory. */
#define VX_EROFS 16        /* The file system is read-only. */
#define VX_EBUSY 17        /* In use, e.g. a directory with something mounted on it. */
#define VX_EXDEV 18        /* Crosses file systems. */
#define VX_EINTR 19        /* Interrupted by a signal. */
#define VX_EPIPE 20        /* Writing to a pipe nobody reads. */
#define VX_ECHILD 21       /* Not a child process. */
#define VX_ESRCH 22        /* No such process. */
#define VX_EAGAIN 23       /* Try again (nothing available right now). */
#define VX_ENOEXEC 24      /* Not a program Vexa can run. */
#define VX_E2BIG 25        /* Arguments and environment too large. */
#define VX_ENOTTY 26       /* Not a terminal. */
#define VX_ESPIPE 27       /* Can't seek on a pipe or terminal. */
#define VX_ELOOP 28        /* Too many symbolic links (or one where none may be). */
#define VX_ETIMEDOUT 29    /* A wait ran out of time. */
#define VX_ENOTSOCK 30     /* Not a socket. */
#define VX_EAFNOSUPPORT 31 /* That address family isn't supported. */
#define VX_EPROTONOSUPPORT 32 /* That protocol isn't supported. */
#define VX_EOPNOTSUPP 33   /* Not something this kind of socket does. */
#define VX_EADDRINUSE 34   /* The address (port or path) is taken. */
#define VX_EADDRNOTAVAIL 35 /* Not one of this machine's addresses. */
#define VX_ENETUNREACH 36  /* No route to that network. */
#define VX_ECONNREFUSED 37 /* Nobody is listening there. */
#define VX_ECONNRESET 38   /* The other side reset the connection. */
#define VX_ENOTCONN 39     /* Not connected. */
#define VX_EISCONN 40      /* Already connected. */
#define VX_EINPROGRESS 41  /* Connecting has started (non-blocking). */
#define VX_EALREADY 42     /* Already connecting. */
#define VX_EMSGSIZE 43     /* Too large for one datagram. */
#define VX_EDESTADDRREQ 44 /* Where to? (no destination given). */
#define VX_ENOPROTOOPT 45  /* No such socket option. */
#define VX_ECONNABORTED 46 /* The connection went away before it was accepted. */
#define VX_EHOSTUNREACH 47 /* The host can't be reached. */

/* Every Vexa program carries an ELF note with this name and type, holding the
 * ABI version as a 32-bit integer. The kernel uses it to tell native programs
 * from Linux ones. */
#define VX_NOTE_NAME "Vexa"
#define VX_NOTE_TYPE_ABI 1
#define VX_ABI_VERSION 1

#endif
