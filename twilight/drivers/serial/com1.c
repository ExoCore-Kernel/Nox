#include <stdbool.h>
#include <stdint.h>

#include <twilight/io.h>
#include <twilight/serial.h>

#define COM1 0x3f8u

bool serial_init(void) {
    outb(COM1 + 1u, 0x00u); /* disable UART interrupts */
    outb(COM1 + 3u, 0x80u); /* enable DLAB */
    outb(COM1 + 0u, 0x03u); /* divisor low: 38400 baud */
    outb(COM1 + 1u, 0x00u); /* divisor high */
    outb(COM1 + 3u, 0x03u); /* 8N1 */
    outb(COM1 + 2u, 0xc7u); /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + 4u, 0x0bu); /* IRQs enabled, RTS/DSR set */

    return true;
}

void serial_write_char(char c) {
    for (uint32_t i = 0; i < 1000000u; ++i) {
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
