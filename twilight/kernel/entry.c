#include <stdint.h>

#include <limine.h>
#include <twilight/font.h>
#include <twilight/font_blob.h>
#include <twilight/framebuffer.h>
#include <twilight/interrupts.h>
#include <twilight/keyboard.h>

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
        __asm__ volatile ("hlt");
    }
}

static __attribute__((noreturn)) void fatal_halt(void) {
    __asm__ volatile ("cli");
    halt_forever();
}

void kmain(void) {
    interrupts_disable();

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        fatal_halt();
    }

    struct limine_framebuffer_response *response = framebuffer_request.response;
    if (response == 0 || response->framebuffer_count == 0 || response->framebuffers == 0) {
        fatal_halt();
    }

    if (!framebuffer_init(response->framebuffers[0])) {
        fatal_halt();
    }

    framebuffer_clear(12, 12, 18);

    if (!font_init(twilight_console_font, twilight_console_font_size)) {
        fatal_halt();
    }

    font_draw_string("Hello, World!", 48, 48, 235, 235, 235);
    font_draw_string("PS/2 keyboard: type below", 48, 48 + font_height() + 2, 160, 200, 255);

    idt_init();
    pic_init();

    if (!ps2_keyboard_init()) {
        font_draw_string("PS/2 keyboard init failed", 48, 48 + (font_height() + 2) * 2, 255, 120, 120);
        fatal_halt();
    }

    interrupts_enable();
    halt_forever();
}
