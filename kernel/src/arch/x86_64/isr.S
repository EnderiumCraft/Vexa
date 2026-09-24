/* Interrupt entry stubs for all 256 vectors. Each pushes a uniform frame (see
 * struct interrupt_frame in vexa/arch.h) and jumps to isr_common. */

.altmacro

/* Vectors 8, 10-14, 17, 21, 29 and 30 have a CPU-pushed error code. For the
 * others, push a zero so every frame has the same layout. */
.macro ISR_STUB vector
isr_stub_\vector:
.if (\vector == 8) || ((\vector >= 10) && (\vector <= 14)) || (\vector == 17) || (\vector == 21) || (\vector == 29) || (\vector == 30)
.else
    pushq $0
.endif
    pushq $\vector
    jmp isr_common
.endm

.macro ISR_TABLE_ENTRY vector
    .quad isr_stub_\vector
.endm

.text
isr_common:
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
    /* Align the stack to 16 bytes for the C ABI, keeping the old rsp. */
    movq %rsp, %rbx
    andq $-16, %rsp
    call interrupt_dispatch
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp /* Drop vector and error code. */
    iretq

.set i, 0
.rept 256
    ISR_STUB %i
    .set i, i + 1
.endr

.section .rodata
.globl isr_stub_table
.balign 8
isr_stub_table:
.set i, 0
.rept 256
    ISR_TABLE_ENTRY %i
    .set i, i + 1
.endr

.section .note.GNU-stack, "", @progbits
