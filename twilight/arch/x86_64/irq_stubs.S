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
    movq 0(%rsp), %rdi
    movq 8(%rsp), %rsi
    movq 16(%rsp), %rdx
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

.macro IRQ_STUB name, handler
.global \name
.type \name, @function
\name:
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
    call \handler
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
.size \name, .-\name
.endm

.macro LEGACY_IRQ_STUB n
.global irq\n\()_stub
.type irq\n\()_stub, @function
irq\n\()_stub:
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
    movq $\n, %rdi
    call legacy_irq_dispatch
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
.size irq\n\()_stub, .-irq\n\()_stub
.endm

/* MSI stubs are intentionally separate from legacy IRQ stubs. The vector is
 * passed directly to the IRQ core and only the Local APIC receives an EOI. */
.macro MSI_IRQ_STUB n
.global msi_irq_\n\()_stub
.type msi_irq_\n\()_stub, @function
msi_irq_\n\()_stub:
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
    movq $\n, %rdi
    call msi_irq_dispatch
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
.size msi_irq_\n\()_stub, .-msi_irq_\n\()_stub
.endm

IRQ_STUB irq0_stub, pit_irq_handler
IRQ_STUB irq1_stub, keyboard_irq_handler
LEGACY_IRQ_STUB 2
LEGACY_IRQ_STUB 3
LEGACY_IRQ_STUB 4
LEGACY_IRQ_STUB 5
LEGACY_IRQ_STUB 6
LEGACY_IRQ_STUB 7
LEGACY_IRQ_STUB 8
LEGACY_IRQ_STUB 9
LEGACY_IRQ_STUB 10
LEGACY_IRQ_STUB 11
LEGACY_IRQ_STUB 12
LEGACY_IRQ_STUB 13
LEGACY_IRQ_STUB 14
LEGACY_IRQ_STUB 15

MSI_IRQ_STUB 64
MSI_IRQ_STUB 65
MSI_IRQ_STUB 66
MSI_IRQ_STUB 67
MSI_IRQ_STUB 68
MSI_IRQ_STUB 69
MSI_IRQ_STUB 70
MSI_IRQ_STUB 71
MSI_IRQ_STUB 72
MSI_IRQ_STUB 73
MSI_IRQ_STUB 74
MSI_IRQ_STUB 75
MSI_IRQ_STUB 76
MSI_IRQ_STUB 77
MSI_IRQ_STUB 78
MSI_IRQ_STUB 79
MSI_IRQ_STUB 80
MSI_IRQ_STUB 81
MSI_IRQ_STUB 82
MSI_IRQ_STUB 83
MSI_IRQ_STUB 84
MSI_IRQ_STUB 85
MSI_IRQ_STUB 86
MSI_IRQ_STUB 87
MSI_IRQ_STUB 88
MSI_IRQ_STUB 89
MSI_IRQ_STUB 90
MSI_IRQ_STUB 91
MSI_IRQ_STUB 92
MSI_IRQ_STUB 93
MSI_IRQ_STUB 94
MSI_IRQ_STUB 95
MSI_IRQ_STUB 96
MSI_IRQ_STUB 97
MSI_IRQ_STUB 98
MSI_IRQ_STUB 99
MSI_IRQ_STUB 100
MSI_IRQ_STUB 101
MSI_IRQ_STUB 102
MSI_IRQ_STUB 103
MSI_IRQ_STUB 104
MSI_IRQ_STUB 105
MSI_IRQ_STUB 106
MSI_IRQ_STUB 107
MSI_IRQ_STUB 108
MSI_IRQ_STUB 109
MSI_IRQ_STUB 110
MSI_IRQ_STUB 111
MSI_IRQ_STUB 112
MSI_IRQ_STUB 113
MSI_IRQ_STUB 114
MSI_IRQ_STUB 115
MSI_IRQ_STUB 116
MSI_IRQ_STUB 117
MSI_IRQ_STUB 118
MSI_IRQ_STUB 119
MSI_IRQ_STUB 120
MSI_IRQ_STUB 121
MSI_IRQ_STUB 122
MSI_IRQ_STUB 123
MSI_IRQ_STUB 124
MSI_IRQ_STUB 125
MSI_IRQ_STUB 126
MSI_IRQ_STUB 127

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

.global msi_irq_stub_table
msi_irq_stub_table:
.quad msi_irq_64_stub, msi_irq_65_stub, msi_irq_66_stub, msi_irq_67_stub, msi_irq_68_stub, msi_irq_69_stub, msi_irq_70_stub, msi_irq_71_stub
.quad msi_irq_72_stub, msi_irq_73_stub, msi_irq_74_stub, msi_irq_75_stub, msi_irq_76_stub, msi_irq_77_stub, msi_irq_78_stub, msi_irq_79_stub
.quad msi_irq_80_stub, msi_irq_81_stub, msi_irq_82_stub, msi_irq_83_stub, msi_irq_84_stub, msi_irq_85_stub, msi_irq_86_stub, msi_irq_87_stub
.quad msi_irq_88_stub, msi_irq_89_stub, msi_irq_90_stub, msi_irq_91_stub, msi_irq_92_stub, msi_irq_93_stub, msi_irq_94_stub, msi_irq_95_stub
.quad msi_irq_96_stub, msi_irq_97_stub, msi_irq_98_stub, msi_irq_99_stub, msi_irq_100_stub, msi_irq_101_stub, msi_irq_102_stub, msi_irq_103_stub
.quad msi_irq_104_stub, msi_irq_105_stub, msi_irq_106_stub, msi_irq_107_stub, msi_irq_108_stub, msi_irq_109_stub, msi_irq_110_stub, msi_irq_111_stub
.quad msi_irq_112_stub, msi_irq_113_stub, msi_irq_114_stub, msi_irq_115_stub, msi_irq_116_stub, msi_irq_117_stub, msi_irq_118_stub, msi_irq_119_stub
.quad msi_irq_120_stub, msi_irq_121_stub, msi_irq_122_stub, msi_irq_123_stub, msi_irq_124_stub, msi_irq_125_stub, msi_irq_126_stub, msi_irq_127_stub
