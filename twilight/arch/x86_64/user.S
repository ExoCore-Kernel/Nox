.section .text

.extern gdt_tss
.extern user_syscall_dispatch

/*
 * Enter a separate user address space without ever running C on an arbitrary
 * bootloader stack after CR3 changes.
 *
 * SysV arguments:
 *   RDI = user RIP
 *   RSI = user RSP
 *   RDX = user CR3 physical address
 *   RCX = kernel CR3 physical address
 */
.global user_mode_enter
.type user_mode_enter, @function
user_mode_enter:
    /* Preserve the kernel ABI state on the original kernel stack. */
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, user_saved_kernel_rsp(%rip)
    movq %rcx, user_saved_kernel_cr3(%rip)

    /* Keep the two user frame values in registers across the CR3 write. */
    movq %rdi, %r8
    movq %rsi, %r9

    /* All user address spaces share Twilight's upper-half kernel mapping, so
     * this dedicated .bss stack remains mapped in both CR3s. Hardware Ring3
     * -> Ring0 transitions use exactly this top through TSS.RSP0. */
    leaq user_transition_stack_end(%rip), %rsp
    andq $-16, %rsp
    movq %rsp, gdt_tss+4(%rip)

    /* Only now leave the kernel address space. No C code executes between this
     * point and IRETQ. */
    movq %rdx, %cr3

    mov $0x1b, %ax
    mov %ax, %ds
    mov %ax, %es

    /* IRETQ frame for CPL3: SS, RSP, RFLAGS, CS, RIP. Keep IF clear during
     * the self-test so no external device IRQ can interleave the transition. */
    pushq $0x1b
    pushq %r9
    pushq $0x2
    pushq $0x23
    pushq %r8
    iretq
.size user_mode_enter, .-user_mode_enter

.global int80_stub
.type int80_stub, @function
int80_stub:
    cld

    /* Preserve all user-visible GPRs across ordinary syscalls. */
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax

    /* 15 pushes leave RSP 8 mod 16; align before the SysV C call. */
    subq $8, %rsp
    movq 8(%rsp), %rdi
    call user_syscall_dispatch
    testl %eax, %eax
    jne .Lreturn_to_kernel

    addq $8, %rsp
    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    iretq

.Lreturn_to_kernel:
    /* Drop alignment + saved user registers + CPU's RIP/CS/RFLAGS/RSP/SS.
     * This returns RSP to user_transition_stack_end. */
    addq $8, %rsp
    addq $120, %rsp
    addq $40, %rsp

    /* Restore the kernel page tables BEFORE touching the original kernel
     * stack. This is what makes the transition safe even if Limine placed its
     * boot stack outside the mappings cloned into the user address space. */
    movq user_saved_kernel_cr3(%rip), %rax
    movq %rax, %cr3

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    movq user_saved_kernel_rsp(%rip), %rsp

    /* Restore the callee-saved kernel state saved by user_mode_enter(). */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
.size int80_stub, .-int80_stub

.section .rodata
.align 16
.global user_probe_blob_start
.global user_probe_blob_end
user_probe_blob_start:
    movabsq $0x54570000, %rax
    int $0x80
    movabsq $0x54570001, %rax
    int $0x80
    ud2
user_probe_blob_end:

.section .bss
.align 4096
user_transition_stack:
    .skip 16384
user_transition_stack_end:

.align 8
user_saved_kernel_rsp:
    .quad 0
user_saved_kernel_cr3:
    .quad 0
