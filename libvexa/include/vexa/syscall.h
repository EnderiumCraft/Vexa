#ifndef LIBVEXA_SYSCALL_H
#define LIBVEXA_SYSCALL_H

#include <stddef.h>
#include <stdint.h>
#include <vexa/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native Vexa system calls. Each returns a negative VX_E* value on error. */

__attribute__((noreturn)) void vx_exit(int code);
long vx_log(const char *text, size_t length);
long vx_yield(void);
long vx_sleep(uint64_t ms);
long vx_process_id(void);
long vx_uptime(void);
/* Seconds since 1970-01-01 UTC, from the real-time clock (0 if unknown). */
long vx_time(void);

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
long vx_rename(const char *from, const char *to);
/* Symbolic links: make one, read where it points (not NUL-terminated;
 * returns the length), or describe the link itself instead of its target. */
long vx_symlink(const char *target, const char *path);
long vx_readlink(const char *path, char *buffer, size_t size);
long vx_lstat(const char *path, struct vx_stat *stat);
/* Changes the access to mapped memory (page aligned): VX_MAP_WRITE, VX_MAP_EXEC. */
long vx_protect(void *address, size_t size, unsigned flags);

/* Threads, at the system call level; <vexa/thread.h> has the easy way. */
long vx_thread_start(const struct vx_thread_start *start);
__attribute__((noreturn)) void vx_thread_exit(int code);
long vx_thread_id(void);
/* Sleeps while *address == expected, until woken (0), timed out (-VX_ETIMEDOUT;
 * -1 waits forever), or interrupted; -VX_EAGAIN if it didn't hold `expected`. */
long vx_wait_address(volatile unsigned *address, unsigned expected, long timeout_ms);
long vx_wake_address(volatile unsigned *address, long count);
long vx_chdir(const char *path);
long vx_getcwd(char *buffer, size_t size);
long vx_pipe(int handles[2]);

/* Processes. */
int vx_spawn(const char *path, const struct vx_spawn *spawn); /* Returns a process handle. */
long vx_wait(int process, unsigned flags);                   /* Returns its exit code. */
long vx_handle_process_id(int process);                      /* The id behind a process handle. */
long vx_kill(long process_id, int signal);
long vx_signal(int signal, int action);
long vx_set_foreground(long group);
long vx_process_list(struct vx_process_info *entries, size_t count);

/* Memory. */
void *vx_map(size_t size, unsigned flags); /* NULL if out of memory. */
long vx_unmap(void *address, size_t size);

/* Sockets (see "Sockets" in abi/vexa/abi.h); <vexa/net.h> has helpers.
 * vx_read and vx_write work on connected sockets too. */
int vx_socket(int family, int type, int protocol); /* Returns a handle. */
long vx_bind(int handle, const struct vx_socket_address *address, size_t length);
long vx_listen(int handle, int backlog);
int vx_accept(int handle, struct vx_socket_address *peer, unsigned flags);
long vx_connect(int handle, const struct vx_socket_address *address, size_t length);
long vx_send(int handle, struct vx_message *message);
long vx_receive(int handle, struct vx_message *message);
long vx_shutdown(int handle, int how);
/* The socket's own address (peer 0) or its peer's; returns the length. */
long vx_socket_address(int handle, int peer, struct vx_socket_address *address);
long vx_socket_pair(int family, int type, int handles[2]);
/* Waits until one of the handles is ready (or timeout_ms passes; -1: no limit). */
long vx_poll(struct vx_poll *handles, size_t count, long timeout_ms);
/* The network interfaces; returns how many there are. */
long vx_net_info(struct vx_net_interface *interfaces, size_t count);

/* Devices and shared files. */
/* A device request (see abi.h: VX_INPUT_*, VX_DISPLAY_*); `arg` is read and
 * written, `size` bytes (at most 256). */
long vx_control(int handle, unsigned request, void *arg, size_t size);
/* Maps a file or device (from `offset`, page aligned), shared: what one
 * mapping writes, the file and every other mapping see. NULL on failure. */
void *vx_map_file(int handle, unsigned long offset, size_t size, unsigned flags);
long vx_resize(int handle, unsigned long size);

/* The system. */
long vx_system_info(struct vx_system_info *info);
/* Restarts or turns off the machine (VX_POWER_RESTART, VX_POWER_OFF). */
long vx_power(int action);
/* The mounted file systems (fills at most `count`); returns how many there are. */
long vx_mounts(struct vx_mount_info *mounts, size_t count);
/* The computer's name, and changing it (letters, digits, '-' and '.'). */
long vx_get_hostname(char *buffer, size_t size);
long vx_set_hostname(const char *name);
/* The calling thread's thread pointer (%fs base): what libvexa keeps per thread. */
long vx_set_thread_pointer(void *address);
/* The devices the kernel knows (see "Devices" in abi/vexa/abi.h): fills in
 * up to `count`, returns how many there are; `generation` (if not NULL)
 * changes whenever the list does. */
long vx_device_list(struct vx_device_info *devices, size_t count, unsigned long long *generation);
/* Another handle to the same object: `new_handle` (closing what was there) or,
 * with -1, the lowest free one. */
long vx_dup(int handle, int new_handle);
/* A process's nice value (-20 runs first, 19 last; 0 by default; children
 * inherit it): set to `nice`, or just read with VX_PRIORITY_GET; `*now`
 * (unless NULL) gets the value. Process id 0 is this one. Returns 0 or a
 * negative VX_E* error. */
long vx_priority(long process_id, int nice, int *now);
long vx_kernel_command(const char *command);

/* A short description of a (negative) VX_E* error. */
const char *vx_strerror(long error);

#ifdef __cplusplus
}
#endif

#endif
