#ifndef VEXA_PERSONALITY_LINUX_H
#define VEXA_PERSONALITY_LINUX_H

#include <stdint.h>

/* Linux x86_64 system call numbers, flags and structure layouts: the Linux
 * ABI as the Linux personality sees it. Kept here, away from the rest of the
 * kernel, which only knows Vexa's own interface. */

/* ---- System call numbers ---- */

#define LINUX_SYS_read 0
#define LINUX_SYS_write 1
#define LINUX_SYS_open 2
#define LINUX_SYS_close 3
#define LINUX_SYS_stat 4
#define LINUX_SYS_fstat 5
#define LINUX_SYS_lstat 6
#define LINUX_SYS_poll 7
#define LINUX_SYS_lseek 8
#define LINUX_SYS_mmap 9
#define LINUX_SYS_mprotect 10
#define LINUX_SYS_munmap 11
#define LINUX_SYS_brk 12
#define LINUX_SYS_rt_sigaction 13
#define LINUX_SYS_rt_sigprocmask 14
#define LINUX_SYS_rt_sigreturn 15
#define LINUX_SYS_ioctl 16
#define LINUX_SYS_pread64 17
#define LINUX_SYS_pwrite64 18
#define LINUX_SYS_readv 19
#define LINUX_SYS_writev 20
#define LINUX_SYS_access 21
#define LINUX_SYS_pipe 22
#define LINUX_SYS_select 23
#define LINUX_SYS_sched_yield 24
#define LINUX_SYS_mremap 25
#define LINUX_SYS_msync 26
#define LINUX_SYS_madvise 28
#define LINUX_SYS_dup 32
#define LINUX_SYS_dup2 33
#define LINUX_SYS_pause 34
#define LINUX_SYS_nanosleep 35
#define LINUX_SYS_alarm 37
#define LINUX_SYS_setitimer 38
#define LINUX_SYS_getpid 39
#define LINUX_SYS_sendfile 40
#define LINUX_SYS_socket 41
#define LINUX_SYS_clone 56
#define LINUX_SYS_fork 57
#define LINUX_SYS_vfork 58
#define LINUX_SYS_execve 59
#define LINUX_SYS_exit 60
#define LINUX_SYS_wait4 61
#define LINUX_SYS_kill 62
#define LINUX_SYS_uname 63
#define LINUX_SYS_fcntl 72
#define LINUX_SYS_flock 73
#define LINUX_SYS_fsync 74
#define LINUX_SYS_fdatasync 75
#define LINUX_SYS_truncate 76
#define LINUX_SYS_ftruncate 77
#define LINUX_SYS_getcwd 79
#define LINUX_SYS_chdir 80
#define LINUX_SYS_fchdir 81
#define LINUX_SYS_rename 82
#define LINUX_SYS_mkdir 83
#define LINUX_SYS_rmdir 84
#define LINUX_SYS_creat 85
#define LINUX_SYS_link 86
#define LINUX_SYS_unlink 87
#define LINUX_SYS_symlink 88
#define LINUX_SYS_readlink 89
#define LINUX_SYS_chmod 90
#define LINUX_SYS_fchmod 91
#define LINUX_SYS_chown 92
#define LINUX_SYS_fchown 93
#define LINUX_SYS_lchown 94
#define LINUX_SYS_umask 95
#define LINUX_SYS_gettimeofday 96
#define LINUX_SYS_getrlimit 97
#define LINUX_SYS_getrusage 98
#define LINUX_SYS_sysinfo 99
#define LINUX_SYS_times 100
#define LINUX_SYS_getuid 102
#define LINUX_SYS_getgid 104
#define LINUX_SYS_setuid 105
#define LINUX_SYS_setgid 106
#define LINUX_SYS_geteuid 107
#define LINUX_SYS_getegid 108
#define LINUX_SYS_setpgid 109
#define LINUX_SYS_getppid 110
#define LINUX_SYS_getpgrp 111
#define LINUX_SYS_setsid 112
#define LINUX_SYS_setreuid 113
#define LINUX_SYS_setregid 114
#define LINUX_SYS_getgroups 115
#define LINUX_SYS_setgroups 116
#define LINUX_SYS_setresuid 117
#define LINUX_SYS_getresuid 118
#define LINUX_SYS_setresgid 119
#define LINUX_SYS_getresgid 120
#define LINUX_SYS_getpgid 121
#define LINUX_SYS_getsid 124
#define LINUX_SYS_rt_sigpending 127
#define LINUX_SYS_rt_sigsuspend 130
#define LINUX_SYS_sigaltstack 131
#define LINUX_SYS_utime 132
#define LINUX_SYS_mknod 133
#define LINUX_SYS_personality 135
#define LINUX_SYS_statfs 137
#define LINUX_SYS_fstatfs 138
#define LINUX_SYS_getpriority 140
#define LINUX_SYS_setpriority 141
#define LINUX_SYS_prctl 157
#define LINUX_SYS_arch_prctl 158
#define LINUX_SYS_setrlimit 160
#define LINUX_SYS_sync 162
#define LINUX_SYS_mount 165
#define LINUX_SYS_umount2 166
#define LINUX_SYS_reboot 169
#define LINUX_SYS_sethostname 170
#define LINUX_SYS_gettid 186
#define LINUX_SYS_tkill 200
#define LINUX_SYS_time 201
#define LINUX_SYS_futex 202
#define LINUX_SYS_sched_getaffinity 204
#define LINUX_SYS_getdents64 217
#define LINUX_SYS_set_tid_address 218
#define LINUX_SYS_clock_gettime 228
#define LINUX_SYS_clock_getres 229
#define LINUX_SYS_clock_nanosleep 230
#define LINUX_SYS_exit_group 231
#define LINUX_SYS_tgkill 234
#define LINUX_SYS_utimes 235
#define LINUX_SYS_openat 257
#define LINUX_SYS_mkdirat 258
#define LINUX_SYS_mknodat 259
#define LINUX_SYS_fchownat 260
#define LINUX_SYS_futimesat 261
#define LINUX_SYS_newfstatat 262
#define LINUX_SYS_unlinkat 263
#define LINUX_SYS_renameat 264
#define LINUX_SYS_linkat 265
#define LINUX_SYS_symlinkat 266
#define LINUX_SYS_readlinkat 267
#define LINUX_SYS_fchmodat 268
#define LINUX_SYS_faccessat 269
#define LINUX_SYS_pselect6 270
#define LINUX_SYS_ppoll 271
#define LINUX_SYS_set_robust_list 273
#define LINUX_SYS_utimensat 280
#define LINUX_SYS_dup3 292
#define LINUX_SYS_pipe2 293
#define LINUX_SYS_prlimit64 302
#define LINUX_SYS_renameat2 316
#define LINUX_SYS_getrandom 318
#define LINUX_SYS_membarrier 324
#define LINUX_SYS_statx 332
#define LINUX_SYS_rseq 334
#define LINUX_SYS_close_range 436
#define LINUX_SYS_faccessat2 439
#define LINUX_SYSCALL_LIMIT 512

/* ---- Error numbers ---- */

#define LE_EPERM 1
#define LE_ENOENT 2
#define LE_ESRCH 3
#define LE_EINTR 4
#define LE_EIO 5
#define LE_E2BIG 7
#define LE_ENOEXEC 8
#define LE_EBADF 9
#define LE_ECHILD 10
#define LE_EAGAIN 11
#define LE_ENOMEM 12
#define LE_EACCES 13
#define LE_EFAULT 14
#define LE_EBUSY 16
#define LE_EEXIST 17
#define LE_EXDEV 18
#define LE_ENODEV 19
#define LE_ENOTDIR 20
#define LE_EISDIR 21
#define LE_EINVAL 22
#define LE_EMFILE 24
#define LE_ENOTTY 25
#define LE_ENOSPC 28
#define LE_ESPIPE 29
#define LE_EROFS 30
#define LE_EPIPE 32
#define LE_ERANGE 34
#define LE_ENAMETOOLONG 36
#define LE_ENOSYS 38
#define LE_ENOTEMPTY 39
#define LE_ELOOP 40
#define LE_ETIMEDOUT 110

/* ---- Files ---- */

#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EMPTY_PATH 0x1000
#define LINUX_RENAME_NOREPLACE 0x1
#define LINUX_CLOSE_RANGE_CLOEXEC 0x4

#define LINUX_O_ACCMODE 03
#define LINUX_O_RDONLY 00
#define LINUX_O_WRONLY 01
#define LINUX_O_RDWR 02
#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_DIRECTORY 0200000
#define LINUX_O_NOFOLLOW 0400000
#define LINUX_O_CLOEXEC 02000000

#define LINUX_F_DUPFD 0
#define LINUX_F_GETFD 1
#define LINUX_F_SETFD 2
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4
#define LINUX_F_GETLK 5
#define LINUX_F_SETLK 6
#define LINUX_F_SETLKW 7
#define LINUX_F_SETOWN 8
#define LINUX_F_GETOWN 9
#define LINUX_F_DUPFD_CLOEXEC 1030
#define LINUX_FD_CLOEXEC 1
#define LINUX_F_UNLCK 2

#define LINUX_S_IFIFO 0010000
#define LINUX_S_IFCHR 0020000
#define LINUX_S_IFDIR 0040000
#define LINUX_S_IFBLK 0060000
#define LINUX_S_IFREG 0100000
#define LINUX_S_IFLNK 0120000

#define LINUX_DT_CHR 2
#define LINUX_DT_DIR 4
#define LINUX_DT_BLK 6
#define LINUX_DT_REG 8
#define LINUX_DT_LNK 10

struct linux_stat {
    uint64_t dev;
    uint64_t ino;
    uint64_t nlink;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t pad0;
    uint64_t rdev;
    int64_t size;
    int64_t blksize;
    int64_t blocks;
    int64_t atime, atime_nsec;
    int64_t mtime, mtime_nsec;
    int64_t ctime, ctime_nsec;
    int64_t reserved[3];
};

struct linux_dirent64 {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    uint8_t type;
    /* The NUL-terminated name follows. */
} __attribute__((packed));

/* ---- Terminals ---- */

#define LINUX_TCGETS 0x5401
#define LINUX_TCSETS 0x5402
#define LINUX_TCSETSW 0x5403
#define LINUX_TCSETSF 0x5404
#define LINUX_TCSBRK 0x5409
#define LINUX_TCXONC 0x540a
#define LINUX_TCFLSH 0x540b
#define LINUX_TIOCSCTTY 0x540e
#define LINUX_TIOCGPGRP 0x540f
#define LINUX_TIOCSPGRP 0x5410
#define LINUX_TIOCGWINSZ 0x5413
#define LINUX_TIOCSWINSZ 0x5414
#define LINUX_FIONREAD 0x541b
#define LINUX_FIONBIO 0x5421
#define LINUX_TIOCNOTTY 0x5422
#define LINUX_TIOCGSID 0x5429
#define LINUX_FIONCLEX 0x5450
#define LINUX_FIOCLEX 0x5451

struct linux_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line;
    uint8_t cc[19];
};

#define LINUX_POLLIN 0x1
#define LINUX_POLLOUT 0x4
#define LINUX_POLLERR 0x8
#define LINUX_POLLHUP 0x10
#define LINUX_POLLNVAL 0x20

struct linux_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

/* ---- Memory and processes ---- */

#define LINUX_PROT_WRITE 0x2
#define LINUX_PROT_EXEC 0x4
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20

#define LINUX_WNOHANG 1
#define LINUX_CLONE_VM 0x100
#define LINUX_CLONE_THREAD 0x10000
#define LINUX_ARCH_SET_FS 0x1002
#define LINUX_ARCH_GET_FS 0x1003
#define LINUX_PR_SET_NAME 15
#define LINUX_PR_GET_NAME 16

#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10

#define LINUX_RLIM_INFINITY (~0ULL)
#define LINUX_RLIMIT_STACK 3
#define LINUX_RLIMIT_NOFILE 7

/* ---- Time ---- */

#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_PROCESS_CPUTIME_ID 2
#define LINUX_CLOCK_THREAD_CPUTIME_ID 3
#define LINUX_CLOCK_REALTIME_COARSE 5
#define LINUX_TIMER_ABSTIME 1

struct linux_timespec {
    int64_t sec;
    int64_t nsec;
};

struct linux_timeval {
    int64_t sec;
    int64_t usec;
};

struct linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    char reserved[4];
};

/* ---- Signals ---- */

#define LINUX_SIG_DFL 0
#define LINUX_SIG_IGN 1
#define LINUX_SIG_BLOCK 0
#define LINUX_SIG_UNBLOCK 1
#define LINUX_SIG_SETMASK 2
#define LINUX_SS_DISABLE 2
#define LINUX_SI_KERNEL 0x80

#define LINUX_SA_RESTORER 0x04000000
#define LINUX_SA_RESTART 0x10000000
#define LINUX_SA_NODEFER 0x40000000
#define LINUX_SA_RESETHAND 0x80000000

#define LINUX_EFLAGS_TF 0x100
#define LINUX_EFLAGS_DF 0x400
/* The flags a program may set (CF PF AF ZF SF TF DF OF RF AC). */
#define LINUX_EFLAGS_USER 0x50dd5

/* The kernel's struct sigaction (not the C library's). */
struct linux_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct linux_sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate; /* NULL: vector registers are kept by the kernel. */
    uint64_t reserved[8];
};

struct linux_ucontext {
    uint64_t flags;
    uint64_t link;
    uint64_t stack_sp;
    int32_t stack_flags;
    int32_t stack_pad;
    uint64_t stack_size;
    struct linux_sigcontext mcontext;
    uint64_t sigmask;
    uint64_t sigmask_rest[15]; /* The C library's sigset_t is 128 bytes. */
};

struct linux_siginfo {
    int32_t signo;
    int32_t errno_value;
    int32_t code;
    int32_t pad;
    uint8_t fields[112];
};

#endif
