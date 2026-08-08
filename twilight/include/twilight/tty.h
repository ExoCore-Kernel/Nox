#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Small early console TTY shared by Linux-compatible userspace and native
 * keyboard/serial drivers. It intentionally has one foreground terminal for
 * bring-up; a real tty/process-session layer can replace it later. */
void tty_reset(void);
void tty_set_active(bool active);
bool tty_is_active(void);

/* IRQ-safe producer used by the PS/2 driver. */
void tty_input_char(char c);
void tty_input_sequence(const char *bytes, size_t length);

/* Poll COM1 and enqueue any received bytes. */
void tty_poll_serial_input(void);
bool tty_input_available(void);
void tty_wait_for_input(void);

/* Blocking single-byte input. Requires the PIT/interrupt path to be alive;
 * it temporarily enables interrupts while waiting so PS/2 IRQ1 can wake it. */
int tty_read_char_blocking(char *out);

/* Console output is mirrored to serial and the framebuffer text cursor. */
void tty_write_char(char c);
void tty_write(const char *bytes, size_t length);
