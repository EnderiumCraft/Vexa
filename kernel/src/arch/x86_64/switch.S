/* void switch_context(uint64_t *save_rsp, uint64_t new_rsp)
 *
 * Saves the callee-saved registers on the current stack, stores the stack
 * pointer in *save_rsp, then switches to new_rsp and restores the registers
 * that were saved there. Returns into whatever the new stack was doing. */
.text
.globl switch_context
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* First code a new thread runs: thread_create() leaves the entry point in r12
 * and its argument in r13 on the initial stack. */
.globl thread_trampoline
thread_trampoline:
    movq %r12, %rdi
    movq %r13, %rsi
    call thread_bootstrap
    ud2 /* thread_bootstrap never returns. */

.section .note.GNU-stack, "", @progbits
