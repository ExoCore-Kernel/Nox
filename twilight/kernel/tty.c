#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/io.h>
#include <twilight/keyboard.h>
#include <twilight/serial.h>
#include <twilight/tty.h>

#define COM1_BASE 0x3f8u
#define COM1_MCR  (COM1_BASE + 4u)
#define COM1_LSR  (COM1_BASE + 5u)
#define COM1_MCR_LOOPBACK 0x10u
#define COM1_LSR_DATA_READY 0x01u

#define TTY_INPUT_CAPACITY 512u

static volatile unsigned int input_head;
static volatile unsigned int input_tail;
static char input_buffer[TTY_INPUT_CAPACITY];
static volatile bool terminal_active;

enum framebuffer_ansi_state {
    FB_ANSI_NORMAL = 0,
    FB_ANSI_ESC,
    FB_ANSI_CSI,
};

static enum framebuffer_ansi_state fb_ansi_state;
static unsigned int fb_csi_value;
static bool fb_csi_has_value;

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

static void prepare_serial_input(void) {
    /* serial_init() may use the 16550 loopback bit while probing COM1. Never
     * let an interactive userspace TTY inherit that state: otherwise bytes we
     * print (including words from the shell banner) can come straight back as
     * stdin. Preserve the other MCR bits and clear only LOOPBACK. */
    const uint8_t mcr = inb(COM1_MCR);
    if ((mcr & COM1_MCR_LOOPBACK) != 0)
        outb(COM1_MCR, (uint8_t)(mcr & (uint8_t)~COM1_MCR_LOOPBACK));

    /* Also discard anything the host terminal/UART queued during boot. Input
     * only becomes meaningful after the foreground TTY is activated. */
    for (unsigned int i = 0; i < 256u; ++i) {
        if ((inb(COM1_LSR) & COM1_LSR_DATA_READY) == 0) break;
        (void)inb(COM1_BASE);
    }
}

static void framebuffer_console_byte(char c) {
    const unsigned char uc = (unsigned char)c;

    if (fb_ansi_state == FB_ANSI_NORMAL) {
        if (uc == 0x1bu) {
            fb_ansi_state = FB_ANSI_ESC;
            return;
        }
        keyboard_console_putc(c);
        return;
    }

    if (fb_ansi_state == FB_ANSI_ESC) {
        if (c == '[') {
            fb_ansi_state = FB_ANSI_CSI;
            fb_csi_value = 0;
            fb_csi_has_value = false;
            return;
        }
        /* Ignore single-character escape sequences on the simple framebuffer
         * mirror; COM1 receives the original bytes unchanged. */
        fb_ansi_state = FB_ANSI_NORMAL;
        return;
    }

    /* CSI parameter bytes. We only need enough cursor behavior to keep the
     * BusyBox line editor readable on the framebuffer. */
    if (c >= '0' && c <= '9') {
        fb_csi_has_value = true;
        if (fb_csi_value < 1000u)
            fb_csi_value = fb_csi_value * 10u + (unsigned int)(c - '0');
        return;
    }
    if (c == ';' || c == '?' || c == '>') return;

    const unsigned int count = fb_csi_has_value && fb_csi_value != 0 ? fb_csi_value : 1u;
    if (c == 'D') {
        for (unsigned int i = 0; i < count; ++i) keyboard_console_putc('\b');
    } else if (c == 'C') {
        for (unsigned int i = 0; i < count; ++i) keyboard_console_putc(' ');
    } else if (c == 'G' || c == 'H') {
        keyboard_console_putc('\r');
    }
    /* K/J/m/h/l and other terminal controls are intentionally ignored by the
     * framebuffer mirror. The serial terminal handles their full semantics. */
    fb_ansi_state = FB_ANSI_NORMAL;
}

void tty_reset(void) {
    const bool was_enabled = interrupts_enabled();
    __asm__ volatile ("cli" ::: "memory");
    input_head = 0;
    input_tail = 0;
    terminal_active = false;
    fb_ansi_state = FB_ANSI_NORMAL;
    fb_csi_value = 0;
    fb_csi_has_value = false;
    if (was_enabled) __asm__ volatile ("sti" ::: "memory");
}

void tty_set_active(bool active) {
    const bool was_enabled = interrupts_enabled();
    __asm__ volatile ("cli" ::: "memory");

    if (active) {
        /* Start every foreground shell with a genuinely empty input stream. */
        input_head = 0;
        input_tail = 0;
        prepare_serial_input();
    }
    terminal_active = active;

    if (was_enabled) __asm__ volatile ("sti" ::: "memory");
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

bool tty_input_available(void) {
    if (!terminal_active) return false;
    const bool was_enabled = interrupts_enabled();
    __asm__ volatile ("cli" ::: "memory");
    tty_poll_serial_input();
    const bool ready = input_head != input_tail;
    if (was_enabled) __asm__ volatile ("sti" ::: "memory");
    return ready;
}

void tty_wait_for_input(void) {
    if (!terminal_active) return;
    for (;;) {
        __asm__ volatile ("cli" ::: "memory");
        tty_poll_serial_input();
        if (input_head != input_tail) return;

        /* Let PIT/PS2 interrupts run while sleeping. PIT guarantees that a
         * serial-only session wakes periodically to poll COM1 as well. */
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
    }
}

int tty_read_char_blocking(char *out) {
    if (out == 0 || !terminal_active) return -1;
    tty_wait_for_input();
    __asm__ volatile ("cli" ::: "memory");
    return queue_pop(out) ? 1 : -1;
}

void tty_write_char(char c) {
    serial_write_char(c);
    framebuffer_console_byte(c);
}

void tty_write(const char *bytes, size_t length) {
    if (bytes == 0) return;
    for (size_t i = 0; i < length; ++i) tty_write_char(bytes[i]);
}
