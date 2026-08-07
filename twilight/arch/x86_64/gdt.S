.section .text

.global twilight_gdt_load
.type twilight_gdt_load, @function
twilight_gdt_load:
    lgdt (%rdi)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    /* Reload CS with a 64-bit far return into Twilight's ring-0 code segment. */
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    mov $0x28, %ax
    ltr %ax
    ret
.size twilight_gdt_load, .-twilight_gdt_load
