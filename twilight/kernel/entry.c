#include <stdint.h>

#include <limine.h>
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

    /* Earliest possible visible marker, independent of the logger. */
    font_draw_string("Twilight booting...", 32, 16, 235, 235, 235);

    const char *cmdline = 0;
    if (cmdline_request.response != 0) {
        cmdline = cmdline_request.response->cmdline;
    }
    const char *os_name = twilight_os_name_from_cmdline(cmdline);

    klog_init();
    twilight_print_version_banner(os_name);
    klog("Framebuffer initialized");
    klog("Console font initialized");

#if TWILIGHT_PANIC_SELF_TEST
    kernel_panic("Panic self-test: renderer is working");
#endif

    idt_init();
    klog("IDT initialized");

    pic_init();
    klog("8259 PIC remapped; keyboard IRQ masked");

    pit_init(1000u);
    klog("PIT configured for 1000 Hz uptime clock");

    interrupts_enable();
    while (timer_ticks() == 0) {
        __asm__ volatile ("hlt");
    }
    klog_enable_uptime();
    klog("PIT IRQ0 active; uptime clock running");

    klog("Initializing PS/2 keyboard");
    if (!ps2_keyboard_init()) {
        kernel_panic("PS/2 keyboard initialization failed or timed out");
    }
    klog("PS/2 keyboard ACK received");

    pic_unmask_irq(1);
    klog("PS/2 keyboard IRQ1 enabled; type below");

    halt_forever();
}
