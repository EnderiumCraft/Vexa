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

/* The system. */
long vx_system_info(struct vx_system_info *info);
long vx_kernel_command(const char *command);

/* A short description of a (negative) VX_E* error. */
const char *vx_strerror(long error);

#endif
