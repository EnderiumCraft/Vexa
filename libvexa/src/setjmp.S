/* setjmp/longjmp: the callee-saved registers, the stack pointer and where
 * setjmp returns to. */
.text
.globl setjmp
.type setjmp, @function
setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rdx      /* The caller's stack pointer, after setjmp returns. */
    movq %rdx, 48(%rdi)
    movq (%rsp), %rdx       /* The return address. */
    movq %rdx, 56(%rdi)
    xorl %eax, %eax
    ret

.globl longjmp
.type longjmp, @function
longjmp:
    movl %esi, %eax
    testl %eax, %eax
    jnz 1f
    incl %eax               /* longjmp(env, 0) makes setjmp return 1. */
1:  movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    jmpq *56(%rdi)

.section .note.GNU-stack, "", @progbits
