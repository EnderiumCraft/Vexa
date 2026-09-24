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

#define VX_ENOSYS 1 /* No such system call. */
#define VX_EFAULT 2 /* A pointer argument was not valid user memory. */
#define VX_EINVAL 3 /* An argument was out of range. */

/* Every Vexa program carries an ELF note with this name and type, holding the
 * ABI version as a 32-bit integer. The kernel uses it to tell native programs
 * from Linux ones. */
#define VX_NOTE_NAME "Vexa"
#define VX_NOTE_TYPE_ABI 1
#define VX_ABI_VERSION 1

#endif
