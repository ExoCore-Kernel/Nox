#include <stdint.h>

#include <limine.h>
#include <twilight/font.h>
#include <twilight/font_blob.h>
#include <twilight/framebuffer.h>
#include <twilight/interrupts.h>
#include <twilight/keyboard.h>
#include <twilight/panic.h>

#ifndef TWILIGHT_PANIC_SELF_TEST
#define TWILIGHT_PANIC_SELF_TEST 0
#endif

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

static void boot_status(const char *text, size_t line, uint8_t r, uint8_t g, uint8_t b) {
    font_draw_string(text, 48, 48 + (font_height() + 2u) * line, r, g, b);
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
    boot_status("[1] framebuffer + font OK", 1, 150, 220, 150);

#if TWILIGHT_PANIC_SELF_TEST
    kernel_panic("Panic self-test: renderer is working");
#endif

    boot_status("[2] loading IDT...", 2, 180, 200, 255);
    idt_init();
    boot_status("[3] IDT OK", 3, 150, 220, 150);

    pic_init();
    boot_status("[4] PIC OK", 4, 150, 220, 150);

    boot_status("[5] initializing PS/2 keyboard...", 5, 180, 200, 255);
    if (!ps2_keyboard_init()) {
        kernel_panic("PS/2 keyboard initialization failed or timed out");
    }
    boot_status("[6] PS/2 keyboard ACK received", 6, 150, 220, 150);

    boot_status("[7] enabling interrupts...", 7, 180, 200, 255);
    interrupts_enable();
    boot_status("[8] interrupts enabled - type below", 8, 150, 220, 150);

    halt_forever();
}
