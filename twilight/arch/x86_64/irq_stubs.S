.section .text

/*
 * Exception stack normalization:
 *   top -> vector, error code, RIP, CS, RFLAGS
 *
 * For exceptions without a CPU-pushed error code we push a synthetic zero.
 * Exceptions that already push an error code only need the vector pushed.
 */
.macro EXC_NOERR n
.global exception_stub_\n
.type exception_stub_\n, @function
exception_stub_\n:
    pushq $0
    pushq $\n
    jmp exception_common
.size exception_stub_\n, .-exception_stub_\n
.endm

.macro EXC_ERR n
.global exception_stub_\n
.type exception_stub_\n, @function
exception_stub_\n:
    pushq $\n
    jmp exception_common
.size exception_stub_\n, .-exception_stub_\n
.endm

exception_common:
    cli
    movq 0(%rsp), %rdi      /* vector */
    movq 8(%rsp), %rsi      /* error code */
    movq 16(%rsp), %rdx     /* RIP */
    andq $-16, %rsp
    call kernel_panic_exception
1:
    hlt
    jmp 1b

EXC_NOERR 0
EXC_NOERR 1
EXC_NOERR 2
EXC_NOERR 3
EXC_NOERR 4
EXC_NOERR 5
EXC_NOERR 6
EXC_NOERR 7
EXC_ERR   8
EXC_NOERR 9
EXC_ERR   10
EXC_ERR   11
EXC_ERR   12
EXC_ERR   13
EXC_ERR   14
EXC_NOERR 15
EXC_NOERR 16
EXC_ERR   17
EXC_NOERR 18
EXC_NOERR 19
EXC_NOERR 20
EXC_ERR   21
EXC_NOERR 22
EXC_NOERR 23
EXC_NOERR 24
EXC_NOERR 25
EXC_NOERR 26
EXC_NOERR 27
EXC_NOERR 28
EXC_ERR   29
EXC_ERR   30
EXC_NOERR 31

.global default_interrupt_stub
.type default_interrupt_stub, @function
default_interrupt_stub:
    pushq $0
    pushq $255
    jmp exception_common
.size default_interrupt_stub, .-default_interrupt_stub

.global irq1_stub
.type irq1_stub, @function
irq1_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %r15
    andq $-16, %rsp
    call keyboard_irq_handler
    movq %r15, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq
.size irq1_stub, .-irq1_stub

.section .rodata
.global exception_stub_table
exception_stub_table:
.quad exception_stub_0, exception_stub_1, exception_stub_2, exception_stub_3
.quad exception_stub_4, exception_stub_5, exception_stub_6, exception_stub_7
.quad exception_stub_8, exception_stub_9, exception_stub_10, exception_stub_11
.quad exception_stub_12, exception_stub_13, exception_stub_14, exception_stub_15
.quad exception_stub_16, exception_stub_17, exception_stub_18, exception_stub_19
.quad exception_stub_20, exception_stub_21, exception_stub_22, exception_stub_23
.quad exception_stub_24, exception_stub_25, exception_stub_26, exception_stub_27
.quad exception_stub_28, exception_stub_29, exception_stub_30, exception_stub_31
