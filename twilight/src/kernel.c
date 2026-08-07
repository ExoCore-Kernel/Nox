#include <stdbool.h>
#include <stdint.h>
#include <limine.h>
#include <twilight/font.h>
#include <twilight/framebuffer.h>

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kmain(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        halt_forever();
    }

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count == 0) {
        halt_forever();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    if (!framebuffer_init(fb)) {
        halt_forever();
    }

    framebuffer_clear(12, 12, 14);

    const char *message = "Hello, World!";
    const size_t text_width = 13u * 18u;
    const size_t x = framebuffer_width() > text_width
        ? (framebuffer_width() - text_width) / 2u
        : 16u;
    const size_t y = framebuffer_height() > 32u
        ? (framebuffer_height() - 32u) / 2u
        : 16u;

    font_draw_string(message, x, y, 224, 224, 224);
    halt_forever();
}
