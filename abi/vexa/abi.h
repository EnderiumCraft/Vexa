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

/* vx_open flags. A handle can only be used the ways it was opened for. */
#define VX_OPEN_READ 0x1
#define VX_OPEN_WRITE 0x2
#define VX_OPEN_CREATE 0x4   /* Create the file if it doesn't exist. */
#define VX_OPEN_TRUNCATE 0x8 /* Empty the file first. */
#define VX_OPEN_APPEND 0x10  /* Every write goes to the end. */

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

/* Every Vexa program carries an ELF note with this name and type, holding the
 * ABI version as a 32-bit integer. The kernel uses it to tell native programs
 * from Linux ones. */
#define VX_NOTE_NAME "Vexa"
#define VX_NOTE_TYPE_ABI 1
#define VX_ABI_VERSION 1

#endif
