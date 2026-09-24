#ifndef LIBVEXA_SYSCALL_H
#define LIBVEXA_SYSCALL_H

#include <stddef.h>
#include <stdint.h>
#include <vexa/abi.h>

/* Native Vexa system calls. Each returns a negative VX_E* value on error. */

__attribute__((noreturn)) void vx_exit(int code);
long vx_log(const char *text, size_t length);
long vx_yield(void);
long vx_sleep(uint64_t ms);
long vx_process_id(void);
long vx_uptime(void);

/* Files. Paths are ordinary C strings here; libvexa passes their length. */
int vx_open(const char *path, unsigned flags); /* Returns a handle. */
long vx_close(int handle);
long vx_read(int handle, void *buffer, size_t size);
long vx_write(int handle, const void *buffer, size_t size);
long vx_seek(int handle, long offset, int whence);
long vx_stat(const char *path, struct vx_stat *stat);
long vx_handle_stat(int handle, struct vx_stat *stat);
long vx_read_dir(int handle, struct vx_dir_entry *entries, size_t count);
long vx_mkdir(const char *path);
long vx_remove(const char *path);

#endif
