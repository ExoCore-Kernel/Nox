#include <stdint.h>

#include <limine.h>
#include <twilight/apic.h>
#include <twilight/font.h>
#include <twilight/font_blob.h>
#include <twilight/framebuffer.h>
#include <twilight/interrupts.h>
#include <twilight/keyboard.h>
#include <twilight/log.h>
#include <twilight/panic.h>
#include <twilight/serial.h>
#include <twilight/timer.h>
#include <twilight/version.h>

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

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
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

static void checkpoint(const char *text, uint64_t line) {
    font_draw_string(text, 32, 16 + line * (font_height() + 2u), 235, 235, 235);
}

void kmain(void) {
    interrupts_disable();
    (void)serial_init();

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

    checkpoint("Twilight booting...", 0);
    checkpoint("A: before cmdline response", 1);

    const char *cmdline = 0;
    if (cmdline_request.response != 0) {
        checkpoint("B: cmdline response present", 2);
        cmdline = cmdline_request.response->cmdline;
        checkpoint("C: cmdline pointer read", 3);
    } else {
        checkpoint("B: no cmdline response", 2);
    }

    const char *os_name = twilight_os_name_from_cmdline(cmdline);
    (void)os_name;
    checkpoint("D: OS name parsed", 4);

    klog_init();
    checkpoint("E: klog_init returned", 5);

    checkpoint("F: entering IDT init", 6);
    idt_init();
    checkpoint("G: IDT initialized", 7);

    if (apic_disable_for_legacy_pic()) {
        checkpoint("H0: local APIC disabled", 8);
    } else {
        checkpoint("H0: APIC not present", 8);
    }

    pic_init();
    checkpoint("H1: PIC initialized", 9);

    pit_init(1000u);
    checkpoint("I: PIT configured", 10);

    interrupts_enable();
    checkpoint("J: interrupts enabled", 11);

    const uint64_t before_soft_irq = timer_ticks();
    __asm__ volatile ("int $0x20");
    if (timer_ticks() > before_soft_irq) {
        checkpoint("K0: software IRQ0 path works", 12);
    } else {
        checkpoint("K0: software IRQ0 path FAILED", 12);
        halt_forever();
    }

    const uint64_t after_soft_irq = timer_ticks();
    while (timer_ticks() == after_soft_irq) {
        __asm__ volatile ("hlt");
    }
    checkpoint("K1: hardware PIT IRQ0 received", 13);

    if (!ps2_keyboard_init()) {
        checkpoint("L: PS/2 init failed", 14);
        kernel_panic("PS/2 keyboard initialization failed or timed out");
    }
    checkpoint("L: PS/2 ACK received", 14);

    pic_unmask_irq(1);
    checkpoint("M: keyboard IRQ1 enabled", 15);

    halt_forever();
}
