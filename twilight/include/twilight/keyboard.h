#pragma once

#include <stdbool.h>

bool ps2_keyboard_init(void);
void keyboard_irq_handler(void);

/* Shared early console sink used by the TTY so Linux userspace output appears
 * on the framebuffer as well as COM1. */
void keyboard_console_putc(char c);
