#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/io.h>
#include <twilight/keyboard.h>
#include <twilight/serial.h>
#include <twilight/tty.h>

#define COM1_BASE 0x3f8u
#define COM1_LSR  (COM1_BASE + 5u)
#define COM1_LSR_DATA_READY 0x01u

#define TTY_INPUT_CAPACITY 512u

static volatile unsigned int input_head;
static volatile unsigned int input_tail;
static char input_buffer[TTY_INPUT_CAPACITY];
static volatile bool terminal_active;

static bool interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ull << 9)) != 0;
}

static bool queue_pop(char *out) {
    if (out == 0) return false;
    const unsigned int tail = input_tail;
    if (tail == input_head) return false;
    *out = input_buffer[tail];
    input_tail = (tail + 1u) % TTY_INPUT_CAPACITY;
    return true;
}

void tty_reset(void) {
    const bool was_enabled = interrupts_enabled();
    __asm__ volatile ("cli" ::: "memory");
    input_head = 0;
    input_tail = 0;
    terminal_active = false;
    if (was_enabled) __asm__ volatile ("sti" ::: "memory");
}

void tty_set_active(bool active) {
    terminal_active = active;
}

bool tty_is_active(void) {
    return terminal_active;
}

void tty_input_char(char c) {
    if (!terminal_active) return;

    /* One empty slot distinguishes full from empty. Drop the newest byte if
     * userspace stops draining input; never corrupt older input from IRQ1. */
    const unsigned int head = input_head;
    const unsigned int next = (head + 1u) % TTY_INPUT_CAPACITY;
    if (next == input_tail) return;
    input_buffer[head] = c;
    input_head = next;
}

void tty_input_sequence(const char *bytes, size_t length) {
    if (bytes == 0) return;
    for (size_t i = 0; i < length; ++i) tty_input_char(bytes[i]);
}

void tty_poll_serial_input(void) {
    if (!terminal_active) return;

    /* QEMU's -serial stdio is a normal 16550A-compatible COM1 device. Drain
     * everything currently waiting in its receive FIFO. */
    for (unsigned int i = 0; i < 64u; ++i) {
        if ((inb(COM1_LSR) & COM1_LSR_DATA_READY) == 0) break;
        char c = (char)inb(COM1_BASE);
        /* Host terminals normally send CR for Return; Linux's default ICRNL
         * input processing presents a newline to applications. */
        if (c == '\r') c = '\n';
        tty_input_char(c);
    }
}

int tty_read_char_blocking(char *out) {
    if (out == 0 || !terminal_active) return -1;

    for (;;) {
        /* SYSCALL enters with IF masked. Keep queue operations atomic against
         * PS/2 IRQ1, and poll the UART while IF is still clear. */
        __asm__ volatile ("cli" ::: "memory");
        tty_poll_serial_input();
        if (queue_pop(out)) return 1;

        /* Let PIT/PS2 interrupts run while sleeping. PIT guarantees that a
         * serial-only session wakes periodically to poll COM1 as well. */
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
    }
}

void tty_write_char(char c) {
    serial_write_char(c);
    keyboard_console_putc(c);
}

void tty_write(const char *bytes, size_t length) {
    if (bytes == 0) return;
    for (size_t i = 0; i < length; ++i) tty_write_char(bytes[i]);
}
