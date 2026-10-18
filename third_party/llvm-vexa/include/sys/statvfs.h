/* <sys/statvfs.h> for building LLVM for Vexa: libvexa has no file system statistics, so these
 * fail (LLVM then says it doesn't know the free space or whether a disk is local). */
#ifndef VEXA_LLVM_SYS_STATVFS_H
#define VEXA_LLVM_SYS_STATVFS_H

#include <errno.h>

#define ST_RDONLY 1
#define ST_NOSUID 2
#define ST_LOCAL 4
#define MNT_LOCAL ST_LOCAL

struct statvfs {
    unsigned long f_bsize, f_frsize;
    unsigned long f_blocks, f_bfree, f_bavail;
    unsigned long f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_flags, f_namemax;
};

static inline int statvfs(const char *path, struct statvfs *buf) {
    (void)path;
    (void)buf;
    errno = ENOSYS;
    return -1;
}
static inline int fstatvfs(int fd, struct statvfs *buf) {
    (void)fd;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

#endif
