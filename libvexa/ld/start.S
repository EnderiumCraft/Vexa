/* Entry point of Vexa's dynamic loader (/lib/vexa-ld.so). The kernel loaded
 * the program and this loader, and starts here with rsp at argc (System V
 * layout); loader_main links the program and returns its entry point, which
 * starts with the same stack, as if the kernel had started it directly. */
.text
.globl _start
.hidden _start
_start:
    xorl %ebp, %ebp
    movq %rsp, %r12          /* Callee-saved: survives loader_main. */
    movq %rsp, %rdi
    andq $-16, %rsp
    call loader_main
    movq %r12, %rsp
    xorl %edx, %edx          /* No exit function to register. */
    jmp *%rax

.section .note.GNU-stack, "", @progbits

/* The loader is a Vexa program too (see abi/vexa/abi.h). */
.section .note.vexa, "a", @note
.balign 4
    .long 5
    .long 4
    .long 1
    .asciz "Vexa"
.balign 4
    .long 1
