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
#define LOG_BOTTOM_MARGIN 24u
#define LOG_BG_R 12u
#define LOG_BG_G 12u
#define LOG_BG_B 18u
#define LOG_FG_R 235u
#define LOG_FG_G 235u
#define LOG_FG_B 235u

static size_t log_y;
static bool uptime_enabled;
static bool heartbeat_enabled;
static bool heartbeat_visible;
static size_t heartbeat_y;
static uint64_t heartbeat_phase;

static size_t line_height(void) {
    return font_height() + 2u;
}

static size_t log_bottom(void) {
    const size_t height = framebuffer_height();
    return height > LOG_BOTTOM_MARGIN ? height - LOG_BOTTOM_MARGIN : height;
}

static void heartbeat_erase(void) {
    if (!heartbeat_enabled || !heartbeat_visible) return;
    const size_t w = font_width();
    const size_t h = font_height();
    if (w == 0 || h == 0) return;
    framebuffer_fill_rect(LOG_X, heartbeat_y, w, h, LOG_BG_R, LOG_BG_G, LOG_BG_B);
    heartbeat_visible = false;
}

static void ensure_room_for_line(void) {
    const size_t line_h = line_height();
    const size_t bottom = log_bottom();

    if (line_h <= 2u || bottom <= LOG_TOP) return;

    while (log_y + line_h + font_height() >= bottom) {
        heartbeat_erase();
        framebuffer_scroll_region_up(LOG_TOP, bottom, line_h,
                                     LOG_BG_R, LOG_BG_G, LOG_BG_B);
        if (log_y >= line_h) log_y -= line_h;
        if (log_y < LOG_TOP) {
            log_y = LOG_TOP;
            break;
        }
    }
}

static void draw_uptime_prefix(uint8_t red, uint8_t green, uint8_t blue) {
    if (!uptime_enabled) {
        font_draw_string("[    0.000000] ", LOG_X, log_y, red, green, blue);
        return;
    }

    const uint64_t us = timer_uptime_us();
    uint64_t seconds = us / 1000000ull;
    uint64_t micros = us % 1000000ull;
    char prefix[16];

    prefix[0] = '[';
    prefix[1] = ' ';
    prefix[2] = ' ';
    prefix[3] = ' ';
    prefix[4] = ' ';
    prefix[5] = '0';
    prefix[6] = '.';
    prefix[7] = '0';
    prefix[8] = '0';
    prefix[9] = '0';
    prefix[10] = '0';
    prefix[11] = '0';
    prefix[12] = '0';
    prefix[13] = ']';
    prefix[14] = ' ';
    prefix[15] = '\0';

    for (int i = 5; i >= 1 && seconds != 0; --i) {
        prefix[i] = (char)('0' + (seconds % 10ull));
        seconds /= 10ull;
    }
    for (int i = 12; i >= 7; --i) {
        prefix[i] = (char)('0' + (micros % 10ull));
        micros /= 10ull;
    }

    font_draw_string(prefix, LOG_X, log_y, red, green, blue);
}

static size_t text_width_pixels(const char *text) {
    if (text == NULL) return 0;
    size_t chars = 0;
    while (text[chars] != '\0') ++chars;
    return chars * font_width();
}

void klog_init(void) {
    log_y = LOG_TOP;
    uptime_enabled = false;
    heartbeat_enabled = false;
    heartbeat_visible = false;
    heartbeat_y = LOG_TOP;
    heartbeat_phase = 0;
}

void klog_enable_uptime(void) {
    uptime_enabled = true;
}

void klog_heartbeat_enable(void) {
    heartbeat_enabled = true;
    heartbeat_visible = false;
    heartbeat_y = log_y;
    heartbeat_phase = timer_uptime_us() / 500000ull;
    klog_heartbeat_update();
}

void klog_heartbeat_update(void) {
    if (!heartbeat_enabled || !uptime_enabled) return;

    const size_t line_h = line_height();
    const size_t bottom = log_bottom();
    if (line_h <= 2u || bottom <= LOG_TOP) return;

    const uint64_t phase = timer_uptime_us() / 500000ull;
    const size_t target_y = log_y;

    if (target_y != heartbeat_y) {
        heartbeat_erase();
        heartbeat_y = target_y;
    }

    if (heartbeat_y + font_height() >= bottom) {
        ensure_room_for_line();
        heartbeat_y = log_y;
    }

    if (phase == heartbeat_phase && heartbeat_visible) return;
    heartbeat_phase = phase;

    heartbeat_erase();
    if ((phase & 1ull) == 0ull) {
        font_draw_string("_", LOG_X, heartbeat_y, LOG_FG_R, LOG_FG_G, LOG_FG_B);
        heartbeat_visible = true;
    }
}

size_t klog_next_console_y(void) {
    ensure_room_for_line();
    heartbeat_y = log_y;
    return log_y;
}

void klog_parts(const char *const *parts, size_t count) {
    const size_t line_h = line_height();
    if (parts == NULL || line_h <= 2u) return;

    heartbeat_erase();
    ensure_room_for_line();
    draw_uptime_prefix(LOG_FG_R, LOG_FG_G, LOG_FG_B);

    size_t x = LOG_X + font_width() * 16u;
    for (size_t i = 0; i < count; ++i) {
        const char *part = parts[i];
        if (part == NULL) continue;
        font_draw_string(part, x, log_y, LOG_FG_R, LOG_FG_G, LOG_FG_B);
        x += text_width_pixels(part);
        serial_write(part);
    }
    serial_write("\n");
    log_y += line_h;
    heartbeat_y = log_y;
}

void klog_color(const char *message, uint8_t red, uint8_t green, uint8_t blue) {
    if (message == NULL) return;

    const size_t line_h = line_height();
    if (line_h <= 2u) return;

    heartbeat_erase();
    ensure_room_for_line();
    draw_uptime_prefix(red, green, blue);
    font_draw_string(message, LOG_X + font_width() * 16u, log_y, red, green, blue);
    log_y += line_h;
    heartbeat_y = log_y;

    serial_write(message);
    serial_write("\n");
}

void klog(const char *message) {
    klog_color(message, LOG_FG_R, LOG_FG_G, LOG_FG_B);
}
