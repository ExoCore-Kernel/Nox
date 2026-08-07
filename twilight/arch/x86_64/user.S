.section .text

.extern gdt_tss
.extern user_syscall_dispatch

.global user_mode_enter
.type user_mode_enter, @function
user_mode_enter:
    /* Preserve the kernel ABI state while CPL3 owns the general registers. */
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* x86_64 TSS.RSP0 is at byte offset 4. This exact RSP becomes the anchor
     * the int80 exit path returns to after discarding the ring-3 frame. */
    movq %rsp, gdt_tss+4(%rip)

    mov $0x1b, %ax
    mov %ax, %ds
    mov %ax, %es

    /* IRETQ frame for CPL3: SS, RSP, RFLAGS, CS, RIP. Keep IF clear during
     * the self-test so no external device IRQ can interleave the transition. */
    pushq $0x1b
    pushq %rsi
    pushq $0x2
    pushq $0x23
    pushq %rdi
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
    /* Drop alignment + saved user registers + CPU's RIP/CS/RFLAGS/RSP/SS. */
    addq $8, %rsp
    addq $120, %rsp
    addq $40, %rsp

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

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
