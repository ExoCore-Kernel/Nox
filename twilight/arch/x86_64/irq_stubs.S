.section .text

.global default_interrupt_stub
.type default_interrupt_stub, @function
default_interrupt_stub:
    cli
1:
    hlt
    jmp 1b
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
