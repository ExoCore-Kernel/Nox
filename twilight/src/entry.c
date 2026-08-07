#include <stdint.h>

#include <limine.h>
#include <twilight/font.h>
#include <twilight/framebuffer.h>

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

static __attribute__((noreturn)) void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void kmain(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        halt_forever();
    }

    struct limine_framebuffer_response *response = framebuffer_request.response;
    if (response == 0 || response->framebuffer_count == 0 || response->framebuffers == 0) {
        halt_forever();
    }

    if (!framebuffer_init(response->framebuffers[0])) {
        halt_forever();
    }

    framebuffer_clear(12, 12, 18);
    font_draw_string("Hello, World!", 48, 48, 235, 235, 235);

    halt_forever();
}
