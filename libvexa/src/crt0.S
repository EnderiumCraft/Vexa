/* Program entry point. The kernel starts us with an empty 16-byte-aligned
 * stack; call main() and pass its return value to vx_exit(). */
.text
.globl _start
_start:
    xorl %ebp, %ebp
    andq $-16, %rsp
    call main
    movl %eax, %edi
    call vx_exit
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
