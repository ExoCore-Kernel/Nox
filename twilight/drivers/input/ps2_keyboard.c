#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/font.h>
#include <twilight/framebuffer.h>
#include <twilight/interrupts.h>
#include <twilight/io.h>
#include <twilight/keyboard.h>

#define PS2_DATA_PORT 0x60u
#define PS2_STATUS_PORT 0x64u
#define PS2_COMMAND_PORT 0x64u
#define PS2_TIMEOUT 1000000u
#define PS2_ACK 0xfau

#define BG_R 12u
#define BG_G 12u
#define BG_B 18u
#define FG_R 235u
#define FG_G 235u
#define FG_B 235u

static bool left_shift;
static bool right_shift;
static bool caps_lock;
static bool extended_prefix;
static size_t cursor_x;
static size_t cursor_y;
static size_t line_start_x;

static const char keymap[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0a]='9',[0x0b]='0',
    [0x0c]='-',[0x0d]='=',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1a]='[',[0x1b]=']',
    [0x1e]='a',[0x1f]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2b]='\\',[0x2c]='z',[0x2d]='x',[0x2e]='c',[0x2f]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' '
};

static const char shift_keymap[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',[0x08]='&',[0x09]='*',[0x0a]='(',[0x0b]=')',
    [0x0c]='_',[0x0d]='+',
    [0x1a]='{',[0x1b]='}',[0x27]=':',[0x28]='"',[0x29]='~',[0x2b]='|',[0x33]='<',[0x34]='>',[0x35]='?'
};

static bool wait_input_clear(void) {
    for (size_t i = 0; i < PS2_TIMEOUT; ++i) {
        if ((inb(PS2_STATUS_PORT) & 0x02u) == 0) return true;
    }
    return false;
}

static bool wait_output_full(void) {
    for (size_t i = 0; i < PS2_TIMEOUT; ++i) {
        if ((inb(PS2_STATUS_PORT) & 0x01u) != 0) return true;
    }
    return false;
}

static void flush_output(void) {
    for (size_t i = 0; i < 32; ++i) {
        if ((inb(PS2_STATUS_PORT) & 0x01u) == 0) break;
        (void)inb(PS2_DATA_PORT);
    }
}

static void console_newline(void) {
    cursor_x = line_start_x;
    cursor_y += font_height() + 2u;
    if (cursor_y + font_height() >= framebuffer_height()) {
        cursor_y = 48u + (font_height() + 2u) * 6u;
        framebuffer_fill_rect(0, cursor_y, framebuffer_width(), framebuffer_height() - cursor_y,
                              BG_R, BG_G, BG_B);
    }
}

static void console_backspace(void) {
    const size_t w = font_width();
    const size_t h = font_height();
    if (w == 0 || cursor_x <= line_start_x) return;
    cursor_x -= w;
    framebuffer_fill_rect(cursor_x, cursor_y, w, h, BG_R, BG_G, BG_B);
}

static void console_putc(char c) {
    const size_t w = font_width();
    const size_t h = font_height();
    if (w == 0 || h == 0) return;

    if (c == '\n') {
        console_newline();
        return;
    }
    if (c == '\b') {
        console_backspace();
        return;
    }

    if (cursor_x + w >= framebuffer_width()) console_newline();
    font_draw_char(c, cursor_x, cursor_y, FG_R, FG_G, FG_B);
    cursor_x += w;
}

static char translate_scancode(uint8_t code) {
    char c = keymap[code];
    const bool shift = left_shift || right_shift;

    if (c >= 'a' && c <= 'z') {
        if (shift != caps_lock) c = (char)(c - 'a' + 'A');
        return c;
    }

    if (shift && shift_keymap[code] != 0) return shift_keymap[code];
    return c;
}

bool ps2_keyboard_init(void) {
    left_shift = false;
    right_shift = false;
    caps_lock = false;
    extended_prefix = false;

    line_start_x = 48u;
    cursor_x = line_start_x;
    cursor_y = 48u + (font_height() + 2u) * 6u;

    flush_output();

    if (!wait_input_clear()) return false;
    outb(PS2_COMMAND_PORT, 0xaeu); /* enable first PS/2 port */

    if (!wait_input_clear()) return false;
    outb(PS2_DATA_PORT, 0xf4u); /* keyboard: enable scanning */

    if (!wait_output_full()) return false;
    if (inb(PS2_DATA_PORT) != PS2_ACK) return false;

    return true;
}

void keyboard_irq_handler(void) {
    const uint8_t status = inb(PS2_STATUS_PORT);
    if ((status & 0x01u) == 0) {
        pic_send_eoi(1);
        return;
    }

    const uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode == 0xe0u) {
        extended_prefix = true;
        pic_send_eoi(1);
        return;
    }

    if (extended_prefix) {
        extended_prefix = false;
        pic_send_eoi(1);
        return;
    }

    const bool released = (scancode & 0x80u) != 0;
    const uint8_t code = (uint8_t)(scancode & 0x7fu);

    if (code == 0x2au) left_shift = !released;
    else if (code == 0x36u) right_shift = !released;
    else if (!released && code == 0x3au) caps_lock = !caps_lock;
    else if (!released && code == 0x1cu) console_putc('\n');
    else if (!released && code == 0x0eu) console_putc('\b');
    else if (!released) {
        const char c = translate_scancode(code);
        if (c != 0) console_putc(c);
    }

    pic_send_eoi(1);
}
