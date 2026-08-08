.section .text

.extern gdt_tss
.extern user_syscall_dispatch
#if TWILIGHT_BUSYBOX_SELF_TEST
.extern linux_busybox_syscall_router
#else
.extern linux_syscall_dispatch
#endif

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
    movq %rdx, user_saved_user_cr3(%rip)

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

    /* Early self-tests deliberately enter CPL3 with IF clear. */
    pushq $0x1b
    pushq %r9
    pushq $0x2
    pushq $0x23
    pushq %r8
    iretq
.size user_mode_enter, .-user_mode_enter

/* Interactive processes need hardware input while running at CPL3. This entry
 * is identical to user_mode_enter except its initial userspace RFLAGS has IF
 * set. SYSCALL saves that flag in R11; FMASK clears IF only while the kernel is
 * servicing the syscall and SYSRETQ restores the userspace value. */
.global user_mode_enter_interruptible
.type user_mode_enter_interruptible, @function
user_mode_enter_interruptible:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, user_saved_kernel_rsp(%rip)
    movq %rcx, user_saved_kernel_cr3(%rip)
    movq %rdx, user_saved_user_cr3(%rip)

    movq %rdi, %r8
    movq %rsi, %r9

    leaq user_transition_stack_end(%rip), %rsp
    andq $-16, %rsp
    movq %rsp, gdt_tss+4(%rip)

    movq %rdx, %cr3

    mov $0x1b, %ax
    mov %ax, %ds
    mov %ax, %es

    pushq $0x1b
    pushq %r9
    pushq $0x202
    pushq $0x23
    pushq %r8
    iretq
.size user_mode_enter_interruptible, .-user_mode_enter_interruptible

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

    jmp .Lrestore_original_kernel_context
.size int80_stub, .-int80_stub

/*
 * Native x86-64 Linux SYSCALL entry.
 *
 * Unlike INT, SYSCALL does not consult the TSS and does not switch RSP. The
 * first instructions therefore touch no stack at all: save the architectural
 * return state in upper-half kernel storage, then move immediately to the
 * transition stack shared by every user CR3.
 *
 * Linux x86-64 input ABI:
 *   RAX = syscall number
 *   RDI, RSI, RDX, R10, R8, R9 = arguments 1..6
 *   RCX = user RIP (written by CPU)
 *   R11 = user RFLAGS (written by CPU)
 */
.global linux_syscall_entry
.type linux_syscall_entry, @function
linux_syscall_entry:
    cld

    movq %rsp, linux_saved_user_rsp(%rip)
    movq %rcx, linux_saved_user_rip(%rip)
    movq %r11, linux_saved_user_rflags(%rip)
    movq %rax, linux_saved_nr(%rip)
    movq %rdi, linux_saved_arg1(%rip)
    movq %rsi, linux_saved_arg2(%rip)
    movq %rdx, linux_saved_arg3(%rip)
    movq %r10, linux_saved_arg4(%rip)
    movq %r8, linux_saved_arg5(%rip)
    movq %r9, linux_saved_arg6(%rip)

    leaq user_transition_stack_end(%rip), %rsp
    andq $-16, %rsp

    /* Kernel C executes on the shared upper-half transition stack while the
     * process CR3 remains active. That keeps validated user pointers directly
     * addressable without giving userspace access to kernel pages. */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es

    /* SysV C wants args 1..6 in RDI,RSI,RDX,RCX,R8,R9 and arg7 on stack. */
    subq $16, %rsp
    movq linux_saved_arg6(%rip), %rax
    movq %rax, 0(%rsp)
    movq linux_saved_nr(%rip), %rdi
    movq linux_saved_arg1(%rip), %rsi
    movq linux_saved_arg2(%rip), %rdx
    movq linux_saved_arg3(%rip), %rcx
    movq linux_saved_arg4(%rip), %r8
    movq linux_saved_arg5(%rip), %r9
#if TWILIGHT_BUSYBOX_SELF_TEST
    call linux_busybox_syscall_router
#else
    call linux_syscall_dispatch
#endif
    addq $16, %rsp

    /* INT64_MIN is Twilight's private "process exited" sentinel. Linux errno
     * values only occupy -1..-4095, so it cannot collide with a syscall result. */
    movabsq $0x8000000000000000, %rdx
    cmpq %rdx, %rax
    je .Llinux_return_to_kernel

    movq %rax, linux_saved_result(%rip)

    /* Linux preserves the argument registers across the entry/exit machinery;
     * RCX and R11 are the architecturally documented SYSCALL clobbers. */
    movq linux_saved_arg1(%rip), %rdi
    movq linux_saved_arg2(%rip), %rsi
    movq linux_saved_arg3(%rip), %rdx
    movq linux_saved_arg4(%rip), %r10
    movq linux_saved_arg5(%rip), %r8
    movq linux_saved_arg6(%rip), %r9

    /* Restore user data selectors before SYSRETQ without destroying RAX. */
    mov $0x1b, %ax
    mov %ax, %ds
    mov %ax, %es
    movq linux_saved_result(%rip), %rax

    movq linux_saved_user_rip(%rip), %rcx
    movq linux_saved_user_rflags(%rip), %r11
    movq linux_saved_user_rsp(%rip), %rsp
    sysretq

.Llinux_return_to_kernel:
    /* exit/exit_group do not return to userspace. Restore the CR3 and original
     * kernel call stack saved by either user-mode entry helper. */
    jmp .Lrestore_original_kernel_context
.size linux_syscall_entry, .-linux_syscall_entry

.Lrestore_original_kernel_context:
    movq user_saved_kernel_cr3(%rip), %rax
    movq %rax, %cr3

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    movq user_saved_kernel_rsp(%rip), %rsp

    /* Restore the callee-saved kernel state saved by the user-mode entry. */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

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
user_saved_user_cr3:
    .quad 0

linux_saved_user_rsp:
    .quad 0
linux_saved_user_rip:
    .quad 0
linux_saved_user_rflags:
    .quad 0
linux_saved_nr:
    .quad 0
linux_saved_arg1:
    .quad 0
linux_saved_arg2:
    .quad 0
linux_saved_arg3:
    .quad 0
linux_saved_arg4:
    .quad 0
linux_saved_arg5:
    .quad 0
linux_saved_arg6:
    .quad 0
linux_saved_result:
    .quad 0
