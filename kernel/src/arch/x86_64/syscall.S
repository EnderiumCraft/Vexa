/* System call entry. The `syscall` instruction jumps here with the user's
 * return address in rcx and flags in r11, still on the user's stack, with
 * interrupts masked (see SFMASK in syscall.c). We switch to the thread's
 * kernel stack and build the same struct interrupt_frame as isr.S, so both
 * paths return through isr_return. */

#define CPU_USER_RSP 8    /* Offsets in struct cpu (vexa/cpu.h). */
#define CPU_KERNEL_RSP 16
#define USER_CS 0x23      /* GDT_USER_CODE | 3 */
#define USER_SS 0x1b      /* GDT_USER_DATA | 3 */
#define SYSCALL_VECTOR 0x100 /* Stored in the frame's vector slot. */

.text
.globl syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:CPU_USER_RSP
    movq %gs:CPU_KERNEL_RSP, %rsp
    pushq $USER_SS
    pushq %gs:CPU_USER_RSP
    pushq %r11
    pushq $USER_CS
    pushq %rcx
    pushq $0
    pushq $SYSCALL_VECTOR
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    movq %rsp, %rdi
    sti
    call syscall_dispatch
    cli
    jmp isr_return

/* void return_to_user(struct interrupt_frame *frame): starts user mode. */
.globl return_to_user
return_to_user:
    movq %rdi, %rsp
    jmp isr_return

.section .note.GNU-stack, "", @progbits
