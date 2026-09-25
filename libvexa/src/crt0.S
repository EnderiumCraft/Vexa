/* Program entry point, linked into every program (also those that use
 * libvexa.so). The kernel, or the dynamic loader, starts us with rsp pointing
 * at argc, then argv[], NULL, envp[], NULL (the System V layout).
 * __libvexa_start(stack, main) does the rest in C. */
.text
.globl _start
_start:
    xorl %ebp, %ebp
    movq %rsp, %rdi
    leaq main(%rip), %rsi
    andq $-16, %rsp
    call __libvexa_start
    ud2

/* The note that marks this as a native Vexa program (see abi/vexa/abi.h). */
.section .note.vexa, "a", @note
.balign 4
    .long 5          /* Name size: "Vexa" plus its terminator. */
    .long 4          /* Descriptor size. */
    .long 1          /* VX_NOTE_TYPE_ABI */
    .asciz "Vexa"
.balign 4
    .long 1          /* VX_ABI_VERSION */

.section .note.GNU-stack, "", @progbits
