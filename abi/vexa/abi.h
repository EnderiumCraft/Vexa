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

struct vx_stat {
    unsigned long long size;
    unsigned long long inode;
    unsigned int type; /* VX_TYPE_* */
    unsigned int links;
    long long modified; /* Seconds since 1970, or 0 if unknown. */
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

/* Every Vexa program carries an ELF note with this name and type, holding the
 * ABI version as a 32-bit integer. The kernel uses it to tell native programs
 * from Linux ones. */
#define VX_NOTE_NAME "Vexa"
#define VX_NOTE_TYPE_ABI 1
#define VX_ABI_VERSION 1

#endif
