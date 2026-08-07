#include <stdbool.h>
#include <stdint.h>

#include <twilight/io.h>
#include <twilight/serial.h>

#define COM1 0x3f8u
#define SERIAL_SPIN_LIMIT 100000u

bool serial_init(void) {
    outb(COM1 + 1u, 0x00u); /* disable UART interrupts */
    outb(COM1 + 3u, 0x80u); /* enable DLAB */
    outb(COM1 + 0u, 0x03u); /* divisor low: 38400 baud */
    outb(COM1 + 1u, 0x00u); /* divisor high */
    outb(COM1 + 3u, 0x03u); /* 8N1 */
    outb(COM1 + 2u, 0xc7u); /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + 4u, 0x03u); /* DTR + RTS; polled output only */
    return true;
}

void serial_write_char(char c) {
    /* Diagnostic output must be reliable, but never able to hang boot forever. */
    for (uint32_t i = 0; i < SERIAL_SPIN_LIMIT; ++i) {
        if ((inb(COM1 + 5u) & 0x20u) != 0) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
}

void serial_write(const char *text) {
    if (text == 0) return;
    while (*text != '\0') {
        if (*text == '\n') serial_write_char('\r');
        serial_write_char(*text++);
    }
}
