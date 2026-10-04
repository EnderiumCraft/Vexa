/* Linked into shared libraries (vexa-cc and vexa-c++ -shared): the library's
 * handle for C++'s __cxa_atexit, as crt0.o has the program's. */
.data
.globl __dso_handle
.hidden __dso_handle
.balign 8
__dso_handle:
    .quad __dso_handle

.section .note.GNU-stack, "", @progbits
