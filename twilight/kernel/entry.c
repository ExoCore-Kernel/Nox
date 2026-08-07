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
    for (;;) __asm__ volatile ("hlt");
}

static __attribute__((noreturn)) void fatal_halt(const char *reason) {
    serial_write("[serial] FATAL: ");
    serial_write(reason);
    serial_write("\n");
    __asm__ volatile ("cli");
    halt_forever();
}

static void trace(const char *message) {
    serial_write("[serial] ");
    serial_write(message);
    serial_write("\n");
}

static void framebuffer_scroll_self_test(void) {
    static const char *lines[] = {
        "Framebuffer scroll test 01", "Framebuffer scroll test 02",
        "Framebuffer scroll test 03", "Framebuffer scroll test 04",
        "Framebuffer scroll test 05", "Framebuffer scroll test 06",
        "Framebuffer scroll test 07", "Framebuffer scroll test 08",
        "Framebuffer scroll test 09", "Framebuffer scroll test 10",
        "Framebuffer scroll test 11", "Framebuffer scroll test 12",
        "Framebuffer scroll test 13", "Framebuffer scroll test 14",
        "Framebuffer scroll test 15", "Framebuffer scroll test 16",
        "Framebuffer scroll test 17", "Framebuffer scroll test 18",
        "Framebuffer scroll test 19", "Framebuffer scroll test 20",
        "Framebuffer scroll test 21", "Framebuffer scroll test 22",
        "Framebuffer scroll test 23", "Framebuffer scroll test 24",
        "Framebuffer scroll test 25", "Framebuffer scroll test 26",
        "Framebuffer scroll test 27", "Framebuffer scroll test 28",
        "Framebuffer scroll test 29", "Framebuffer scroll test 30",
        "Framebuffer scroll test 31", "Framebuffer scroll test 32",
        "Framebuffer scroll test 33", "Framebuffer scroll test 34",
        "Framebuffer scroll test 35", "Framebuffer scroll test 36",
        "Framebuffer scroll test 37", "Framebuffer scroll test 38",
        "Framebuffer scroll test 39", "Framebuffer scroll test 40",
        "Framebuffer scroll test 41", "Framebuffer scroll test 42",
        "Framebuffer scroll test 43", "Framebuffer scroll test 44",
        "Framebuffer scroll test 45", "Framebuffer scroll test 46",
        "Framebuffer scroll test 47", "Framebuffer scroll test 48",
        "Framebuffer scroll test 49", "Framebuffer scroll test 50",
        "Framebuffer scroll test 51", "Framebuffer scroll test 52",
        "Framebuffer scroll test 53", "Framebuffer scroll test 54",
        "Framebuffer scroll test 55", "Framebuffer scroll test 56",
        "Framebuffer scroll test 57", "Framebuffer scroll test 58",
        "Framebuffer scroll test 59", "Framebuffer scroll test 60"
    };

    trace("starting framebuffer scroll self-test");
    klog("Starting framebuffer scroll self-test (500 ms per line)");
    for (uint64_t i = 0; i < (sizeof(lines) / sizeof(lines[0])); ++i) {
        klog(lines[i]);
        timer_sleep_ms(500u);
    }
    klog("Framebuffer scroll self-test complete");
    trace("framebuffer scroll self-test complete");
}

void kmain(void) {
    interrupts_disable();
    (void)serial_init();
    trace("entered kmain");

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        fatal_halt("unsupported Limine base revision");
    }
    trace("Limine base revision OK");

    struct limine_framebuffer_response *response = framebuffer_request.response;
    if (response == 0 || response->framebuffer_count == 0 || response->framebuffers == 0) {
        fatal_halt("framebuffer response missing");
    }
    trace("framebuffer response present");

    if (!framebuffer_init(response->framebuffers[0])) {
        fatal_halt("framebuffer_init failed");
    }
    trace("framebuffer initialized");

    framebuffer_clear(12, 12, 18);
    trace("framebuffer cleared");

    if (!font_init(twilight_console_font, twilight_console_font_size)) {
        fatal_halt("font_init failed");
    }
    trace("font initialized");

    const char *cmdline = 0;
    if (cmdline_request.response != 0) {
        cmdline = cmdline_request.response->cmdline;
        trace("Limine cmdline response present");
    } else {
        trace("no Limine cmdline response");
    }

    const char *os_name = twilight_os_name_from_cmdline(cmdline);
    trace("OS name parsed");

    klog_init();
    trace("klog initialized");

    trace("printing Twilight version banner");
    twilight_print_version_banner(os_name);
    trace("version banner returned");

    klog("Framebuffer initialized");
    klog("Console font initialized");
    trace("initial framebuffer logs returned");

#if TWILIGHT_PANIC_SELF_TEST
    kernel_panic("Panic self-test: renderer is working");
#endif

    idt_init();
    trace("IDT initialized");
    klog("IDT initialized");

    if (apic_disable_for_legacy_pic()) {
        trace("local APIC disabled");
        klog("Local APIC disabled for legacy PIC bring-up");
    } else {
        trace("local APIC not present");
        klog("Local APIC not present; using legacy PIC");
    }

    pic_init();
    trace("8259 PIC initialized");
    klog("8259 PIC initialized; IRQ0 unmasked, IRQ1 masked");

    pit_init(1000u);
    trace("PIT configured");
    klog("PIT configured for 1000 Hz uptime clock");

    interrupts_enable();
    trace("interrupts enabled; waiting for PIT IRQ0");
    while (timer_ticks() == 0) __asm__ volatile ("hlt");
    trace("first hardware PIT IRQ0 received");

    klog_enable_uptime();
    klog("PIT IRQ0 active; uptime clock running");

    framebuffer_scroll_self_test();

    trace("initializing PS/2 keyboard");
    klog("Initializing PS/2 keyboard");
    if (!ps2_keyboard_init()) {
        trace("PS/2 keyboard initialization failed");
        kernel_panic("PS/2 keyboard initialization failed or timed out");
    }
    trace("PS/2 keyboard ACK received");
    klog("PS/2 keyboard ACK received");

    pic_unmask_irq(1);
    trace("PS/2 IRQ1 unmasked; boot complete");
    klog("PS/2 keyboard IRQ1 enabled; type below");

    halt_forever();
}
