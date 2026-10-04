/* Program entry point, linked into every program (also those that use
 * libvexa.so). The kernel, or the dynamic loader, starts us with rsp pointing
 * at argc, then argv[], NULL, envp[], NULL (the System V layout).
 * __libvexa_start_init(stack, main, the program's .init_array and
 * .fini_array) does the rest in C. */
.text
.globl _start
_start:
    xorl %ebp, %ebp
    movq %rsp, %rdi
    leaq main(%rip), %rsi
    leaq __init_array_start(%rip), %rdx
    leaq __init_array_end(%rip), %rcx
    leaq __fini_array_start(%rip), %r8
    leaq __fini_array_end(%rip), %r9
    andq $-16, %rsp
    call __libvexa_start_init
    ud2

/* C++: this module's handle, for __cxa_atexit (normally in crtbegin.o). */
.data
.globl __dso_handle
.hidden __dso_handle
.balign 8
__dso_handle:
    .quad __dso_handle

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
