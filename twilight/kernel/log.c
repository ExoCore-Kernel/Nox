#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/font.h>
#include <twilight/framebuffer.h>
#include <twilight/log.h>
#include <twilight/serial.h>
#include <twilight/timer.h>

#define LOG_X 32u
#define LOG_TOP 32u
#define LOG_BG_R 12u
#define LOG_BG_G 12u
#define LOG_BG_B 18u
#define LOG_FG_R 235u
#define LOG_FG_G 235u
#define LOG_FG_B 235u

static size_t log_y;
static bool uptime_enabled;

static void append_char(char *buffer, size_t *index, size_t capacity, char c) {
    if (*index + 1u >= capacity) return;
    buffer[(*index)++] = c;
    buffer[*index] = '\0';
}

static void append_u64_padded(char *buffer, size_t *index, size_t capacity,
                              uint64_t value, unsigned width, char pad) {
    char temp[24];
    unsigned n = 0;
    do {
        temp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && n < sizeof(temp));

    while (n < width) {
        append_char(buffer, index, capacity, pad);
        --width;
    }
    while (n > 0) append_char(buffer, index, capacity, temp[--n]);
}

static void format_prefix(char out[32]) {
    const uint64_t us = uptime_enabled ? timer_uptime_us() : 0;
    const uint64_t seconds = us / 1000000ull;
    const uint64_t micros = us % 1000000ull;
    size_t i = 0;

    append_char(out, &i, 32, '[');
    append_u64_padded(out, &i, 32, seconds, 5, ' ');
    append_char(out, &i, 32, '.');
    append_u64_padded(out, &i, 32, micros, 6, '0');
    append_char(out, &i, 32, ']');
    append_char(out, &i, 32, ' ');
}

void klog_init(void) {
    log_y = LOG_TOP;
    uptime_enabled = false;
}

void klog_enable_uptime(void) {
    uptime_enabled = true;
}

void klog_color(const char *message, uint8_t red, uint8_t green, uint8_t blue) {
    if (message == NULL) return;

    char prefix[32] = {0};
    format_prefix(prefix);

    /* Always mirror logs to COM1 so framebuffer failures remain debuggable. */
    serial_write(prefix);
    serial_write(message);
    serial_write("\n");

    const size_t line_h = font_height() + 2u;
    if (line_h == 2u) return;

    if (log_y + font_height() >= framebuffer_height()) {
        framebuffer_fill_rect(0, LOG_TOP, framebuffer_width(), framebuffer_height() - LOG_TOP,
                              LOG_BG_R, LOG_BG_G, LOG_BG_B);
        log_y = LOG_TOP;
    }

    font_draw_string(prefix, LOG_X, log_y, red, green, blue);
    font_draw_string(message, LOG_X + font_width() * 15u, log_y, red, green, blue);
    log_y += line_h;
}

void klog(const char *message) {
    klog_color(message, LOG_FG_R, LOG_FG_G, LOG_FG_B);
}
