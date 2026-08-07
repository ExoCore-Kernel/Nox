#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/font.h>
#include <twilight/framebuffer.h>
#include <twilight/log.h>
#include <twilight/serial.h>
#include <twilight/timer.h>

#define LOG_X 32u
#define LOG_TOP 40u
#define LOG_BG_R 12u
#define LOG_BG_G 12u
#define LOG_BG_B 18u
#define LOG_FG_R 235u
#define LOG_FG_G 235u
#define LOG_FG_B 235u

static size_t log_y;
static bool uptime_enabled;

void klog_init(void) {
    log_y = LOG_TOP;
    uptime_enabled = false;
}

void klog_enable_uptime(void) {
    uptime_enabled = true;
}

static void draw_prefix(uint8_t red, uint8_t green, uint8_t blue) {
    /* Safe early prefix: no stack buffers, no formatting helpers. */
    if (!uptime_enabled) {
        font_draw_string("[    0.000000] ", LOG_X, log_y, red, green, blue);
        return;
    }

    /* Temporary fixed-width uptime rendering, intentionally simple. */
    const uint64_t us = timer_uptime_us();
    const uint64_t seconds = us / 1000000ull;
    const uint64_t micros = us % 1000000ull;
    char prefix[16] = "[00000.000000]";

    uint64_t s = seconds;
    for (int i = 5; i >= 1; --i) {
        prefix[i] = (char)('0' + (s % 10u));
        s /= 10u;
    }
    uint64_t m = micros;
    for (int i = 12; i >= 7; --i) {
        prefix[i] = (char)('0' + (m % 10u));
        m /= 10u;
    }
    font_draw_string(prefix, LOG_X, log_y, red, green, blue);
    font_draw_string(" ", LOG_X + font_width() * 15u, log_y, red, green, blue);
}

void klog_color(const char *message, uint8_t red, uint8_t green, uint8_t blue) {
    if (message == NULL) return;

    const size_t line_h = font_height() + 2u;
    if (line_h == 2u) return;

    if (log_y + font_height() >= framebuffer_height()) {
        framebuffer_fill_rect(0, LOG_TOP, framebuffer_width(), framebuffer_height() - LOG_TOP,
                              LOG_BG_R, LOG_BG_G, LOG_BG_B);
        log_y = LOG_TOP;
    }

    draw_prefix(red, green, blue);
    font_draw_string(message, LOG_X + font_width() * 16u, log_y, red, green, blue);
    log_y += line_h;

    /* Best-effort serial mirroring after framebuffer output. */
    serial_write(message);
    serial_write("\n");
}

void klog(const char *message) {
    klog_color(message, LOG_FG_R, LOG_FG_G, LOG_FG_B);
}
