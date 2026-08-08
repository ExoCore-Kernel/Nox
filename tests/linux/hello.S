.section .text
.global _start
.type _start, @function
_start:
    /* Linux x86-64: write(1, message, message_len). */
    movq $1, %rax
    movq $1, %rdi
    leaq message(%rip), %rsi
    movq $message_len, %rdx
    syscall

    /* Linux x86-64: exit_group(0). */
    movq $231, %rax
    xorl %edi, %edi
    syscall

    /* exit_group never returns on Linux or Twilight. */
    ud2
.size _start, .-_start

.section .rodata
message:
    .ascii "Hello from an x86-64 Linux ELF running on Twilight!\n"
message_end:
.equ message_len, message_end - message
