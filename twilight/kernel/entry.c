#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <limine.h>
#include <twilight/acpi.h>
#include <twilight/apic.h>
#include <twilight/font.h>
#include <twilight/font_blob.h>
#include <twilight/framebuffer.h>
#include <twilight/gdt.h>
#include <twilight/heap.h>
#include <twilight/interrupts.h>
#include <twilight/keyboard.h>
#include <twilight/linux_compat.h>
#include <twilight/log.h>
#include <twilight/mmio.h>
#include <twilight/panic.h>
#include <twilight/pmm.h>
#include <twilight/security.h>
#include <twilight/serial.h>
#include <twilight/timer.h>
#include <twilight/tpm.h>
#include <twilight/user.h>
#include <twilight/version.h>
#include <twilight/vmm.h>

#ifndef TWILIGHT_PANIC_SELF_TEST
#define TWILIGHT_PANIC_SELF_TEST 0
#endif

#ifndef TWILIGHT_SCROLL_SELF_TEST
#define TWILIGHT_SCROLL_SELF_TEST 0
#endif

#ifndef TWILIGHT_PMM_SELF_TEST
#define TWILIGHT_PMM_SELF_TEST 1
#endif

#ifndef TWILIGHT_VMM_SELF_TEST
#define TWILIGHT_VMM_SELF_TEST 1
#endif

#ifndef TWILIGHT_HEAP_SELF_TEST
#define TWILIGHT_HEAP_SELF_TEST 1
#endif

#ifndef TWILIGHT_USERMODE_SELF_TEST
#define TWILIGHT_USERMODE_SELF_TEST 1
#endif

#ifndef TWILIGHT_LINUX_COMPAT_SELF_TEST
#define TWILIGHT_LINUX_COMPAT_SELF_TEST 1
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

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
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

static void u64_to_decimal(uint64_t value, char out[32]) {
    char reverse[32];
    size_t count = 0;

    do {
        reverse[count++] = (char)('0' + (value % 10ull));
        value /= 10ull;
    } while (value != 0 && count < sizeof(reverse));

    size_t i = 0;
    while (count != 0) out[i++] = reverse[--count];
    out[i] = '\0';
}

static void spin_pause(uint64_t iterations) {
    for (volatile uint64_t i = 0; i < iterations; ++i) {
        __asm__ volatile ("pause");
    }
}

static bool wait_for_first_pit_irq(void) {
    for (uint64_t i = 0; i < 10000000ull; ++i) {
        if (timer_ticks() != 0) return true;
        __asm__ volatile ("pause");
    }
    return timer_ticks() != 0;
}

static void trace_irq_state(const char *phase) {
    char mask[32];
    char irr[32];
    char isr[32];
    char pit[32];
    char ticks[32];
    char iflag[32];

    u64_to_decimal(pic_master_mask(), mask);
    u64_to_decimal(pic_master_irr(), irr);
    u64_to_decimal(pic_master_isr(), isr);
    u64_to_decimal(pit_read_counter(), pit);
    u64_to_decimal(timer_ticks(), ticks);
    u64_to_decimal(interrupts_are_enabled() ? 1u : 0u, iflag);

    serial_write("[serial] IRQ diag ");
    serial_write(phase);
    serial_write(": IF=");
    serial_write(iflag);
    serial_write(" PIC-mask=");
    serial_write(mask);
    serial_write(" IRR=");
    serial_write(irr);
    serial_write(" ISR=");
    serial_write(isr);
    serial_write(" PIT-count=");
    serial_write(pit);
    serial_write(" ticks=");
    serial_write(ticks);
    serial_write("\n");
}

static void log_pmm_stats(void) {
    struct pmm_stats stats;
    char reported_mib[32];
    char usable_mib[32];
    char free_mib[32];
    char metadata_kib[32];
    char pinned_pages[32];

    pmm_get_stats(&stats);
    u64_to_decimal(stats.reported_bytes / (1024ull * 1024ull), reported_mib);
    u64_to_decimal(stats.usable_bytes / (1024ull * 1024ull), usable_mib);
    u64_to_decimal(stats.free_bytes / (1024ull * 1024ull), free_mib);
    u64_to_decimal((stats.metadata_pages * TWILIGHT_PAGE_SIZE) / 1024ull, metadata_kib);
    u64_to_decimal(stats.pinned_pages, pinned_pages);

    const char *memory_parts[] = {
        "Memory: ", reported_mib, " MiB reported, ", usable_mib, " MiB usable"
    };
    klog_parts(memory_parts, sizeof(memory_parts) / sizeof(memory_parts[0]));

    const char *pmm_parts[] = {
        "PMM: ", free_mib, " MiB free; metadata ", metadata_kib,
        " KiB; pinned pages ", pinned_pages
    };
    klog_parts(pmm_parts, sizeof(pmm_parts) / sizeof(pmm_parts[0]));
}

static void log_heap_stats(void) {
    struct heap_stats stats;
    char arena_mib[32];
    char mapped_pages[32];

    heap_get_stats(&stats);
    u64_to_decimal(stats.arena_bytes / (1024ull * 1024ull), arena_mib);
    u64_to_decimal(stats.mapped_pages, mapped_pages);

    const char *parts[] = {
        "Kernel heap initialized: ", arena_mib,
        " MiB virtual arena; ", mapped_pages, " anchor page(s) mapped"
    };
    klog_parts(parts, sizeof(parts) / sizeof(parts[0]));
}

static void initialize_acpi(void) {
    if (rsdp_request.response == 0 || rsdp_request.response->address == 0) {
        trace("ACPI RSDP unavailable");
        klog("ACPI RSDP unavailable; firmware table discovery limited");
        return;
    }

    if (!acpi_init(rsdp_request.response->address)) {
        trace("ACPI table validation failed");
        klog("ACPI tables were supplied but failed validation");
        return;
    }

    trace("ACPI tables initialized");
    klog("ACPI RSDT/XSDT discovery initialized");
}

static void initialize_tpm_security(void) {
    trace("probing TPM 2.0 Root of Trust");
    if (!tpm_init()) {
        trace("TPM initialization returned failure");
        klog("TPM initialization failed, unable to establish Root of Trust. Security may be impacted.");
        return;
    }

    struct tpm_status tpm;
    tpm_get_status(&tpm);

    if (!tpm.detected) {
        trace("TPM chip not detected; Root of Trust unavailable");
        klog("TPM chip not detected, unable to establish Root of Trust. Security may be impacted.");
        return;
    }

    char start_method[32];
    u64_to_decimal(tpm.start_method, start_method);

    if (!tpm.transport_ready) {
        const char *parts[] = {
            "TPM chip detected (ACPI start method ", start_method,
            "), but no supported TPM 2.0 CRB transport is usable; unable to establish Root of Trust. Security may be impacted."
        };
        klog_parts(parts, sizeof(parts) / sizeof(parts[0]));
        trace("TPM detected but Root of Trust transport unavailable");
        return;
    }

    klog("TPM 2.0 CRB transport ready; measuring Twilight security state");
    trace("extending Twilight measurements and creating PCR policy");
    if (!security_establish_tpm_root_of_trust()) {
        tpm_get_status(&tpm);
        char response_code[32];
        u64_to_decimal(tpm.last_response_code, response_code);
        const char *parts[] = {
            "TPM chip detected, but unable to establish cryptographic Root of Trust; TPM response code ",
            response_code, ". Security may be impacted."
        };
        klog_parts(parts, sizeof(parts) / sizeof(parts[0]));
        trace("TPM Root of Trust establishment failed");
        return;
    }

    tpm_get_status(&tpm);
    if (!tpm.root_of_trust_established || !tpm.pcr_policy_bound || !tpm.vault_ready) {
        kernel_panic("TPM reported incomplete Root of Trust after establishment");
    }

    trace("running authenticated TPM Root of Trust vault self-test");
    if (!tpm_vault_self_test()) {
        kernel_panic("Established TPM Root of Trust failed authenticated vault self-test");
    }

    trace("TPM Root of Trust self-test passed");
    klog("TPM Root of Trust established: PCR 7 firmware policy + PCR 11 Twilight kernel + PCR 12 security policy");
    klog("TPM kernel vault active: PCR-bound AES-128-CFB + HMAC-SHA256 keys remain inside TPM");
    klog("TPM policy is fail-closed: changing the bound PCR state revokes access to protected kernel secrets");
    klog("Verified-boot boundary: firmware/bootloader must authenticate Twilight before kmain; TPM anchors measured state and kernel secrets from this point onward");
}

#if TWILIGHT_SCROLL_SELF_TEST
static void framebuffer_scroll_self_test(void) {
    trace("starting framebuffer scroll self-test");
    klog("Starting framebuffer scroll self-test (500 ms per line)");

    for (uint64_t i = 1; i <= 60; ++i) {
        char number[32];
        u64_to_decimal(i, number);
        const char *parts[] = { "Framebuffer scroll test ", number };
        klog_parts(parts, sizeof(parts) / sizeof(parts[0]));
        timer_sleep_ms(500u);
        klog_heartbeat_update();
    }

    klog("Framebuffer scroll self-test complete");
    trace("framebuffer scroll self-test complete");
}
#endif

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

    if (memmap_request.response == 0 || hhdm_request.response == 0) {
        kernel_panic("Limine memory map or HHDM response missing");
    }

    trace("initializing physical memory manager");
    if (!pmm_init(memmap_request.response, hhdm_request.response->offset)) {
        kernel_panic("Physical memory manager initialization failed");
    }
    trace("physical memory manager initialized");
    klog("Physical memory manager initialized");
    log_pmm_stats();

#if TWILIGHT_PMM_SELF_TEST
    trace("running PMM self-test");
    if (!pmm_self_test()) kernel_panic("Physical memory manager self-test failed");
    trace("PMM self-test passed");
    klog("PMM self-test passed: page, aligned DMA32 run, free and double-free guard");
#endif

    trace("initializing virtual memory manager");
    if (!vmm_init()) kernel_panic("x86_64 virtual memory manager initialization failed");
    trace("virtual memory manager initialized");
    if (vmm_nx_supported()) klog("x86_64 VMM initialized: 4-level paging, NX enabled");
    else klog("x86_64 VMM initialized: 4-level paging, NX unavailable");

#if TWILIGHT_VMM_SELF_TEST
    trace("running VMM self-test");
    if (!vmm_self_test()) kernel_panic("Virtual memory manager self-test failed");
    trace("VMM self-test passed");
    klog("VMM self-test passed: map, translate, protect, unmap and address-space clone");
#endif

    trace("initializing kernel heap");
    if (!heap_init()) kernel_panic("Kernel heap initialization failed");
    trace("kernel heap initialized");
    log_heap_stats();

#if TWILIGHT_HEAP_SELF_TEST
    trace("running kernel heap self-test");
    if (!heap_self_test()) kernel_panic("Kernel heap self-test failed");
    trace("kernel heap self-test passed");
    klog("Heap self-test passed: slab, calloc, realloc, large pages and leak checks");
#endif

    trace("initializing MMIO mapper");
    if (!mmio_init()) kernel_panic("Kernel MMIO mapper initialization failed");
    trace("MMIO mapper initialized");
    klog("MMIO mapper initialized for cache-disabled device mappings");

    initialize_acpi();

#if TWILIGHT_LINUX_COMPAT_SELF_TEST
    trace("running Linux compatibility self-test");
    if (!linux_compat_self_test()) kernel_panic("Linux compatibility layer self-test failed");
    trace("Linux compatibility self-test passed");
    klog("Linux compatibility layer ready: types, list, slab, printk and MMIO basics");
#endif

    trace("initializing x86_64 GDT and TSS");
    if (!gdt_init()) kernel_panic("x86_64 GDT/TSS initialization failed");
    trace("GDT and TSS initialized");
    klog("x86_64 GDT/TSS initialized: ring 0 and ring 3 segments ready");

#if TWILIGHT_PANIC_SELF_TEST
    kernel_panic("Panic self-test: renderer is working");
#endif

    idt_init();
    trace("IDT initialized");
    klog("IDT initialized; DPL3 int 0x80 syscall gate installed");

#if TWILIGHT_USERMODE_SELF_TEST
    trace("entering ring 3 self-test");
    if (!user_mode_self_test()) kernel_panic("Ring 3 privilege transition self-test failed");
    trace("ring 3 self-test passed");
    klog("Ring 3 transition passed: CPL3 -> int80 -> CPL0 -> CPL3 -> kernel");
#endif

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

    trace("testing IRQ0 vector with software INT 0x20");
    __asm__ volatile ("int $0x20");
    if (timer_ticks() != 1) fatal_halt("software IRQ0 vector test did not reach PIT handler");
    trace("software IRQ0 vector test passed");

    pit_init(1000u);
    trace("PIT configured");
    klog("PIT configured for 1000 Hz uptime clock");

    const uint16_t pit_before = pit_read_counter();
    spin_pause(250000u);
    const uint16_t pit_after = pit_read_counter();
    if (pit_before == pit_after) trace("warning: PIT counter did not change during pre-STI probe");
    else trace("PIT counter is running before interrupts are enabled");
    trace_irq_state("before STI");

    interrupts_enable();
    trace("interrupts enabled; waiting for PIT IRQ0");

    if (!wait_for_first_pit_irq()) {
        trace_irq_state("direct PIC timeout");
        interrupts_disable();

        trace("direct PIC delivery failed; trying Local APIC virtual-wire ExtINT fallback");
        if (!apic_enable_virtual_wire_for_legacy_pic()) {
            fatal_halt("could not configure Local APIC virtual-wire fallback");
        }

        pic_init();
        pit_init(1000u);
        trace_irq_state("virtual-wire configured before STI");

        interrupts_enable();
        if (!wait_for_first_pit_irq()) {
            trace_irq_state("virtual-wire timeout");
            fatal_halt("PIT IRQ0 failed in both direct-PIC and APIC virtual-wire modes");
        }

        trace("PIT IRQ0 recovered through Local APIC virtual-wire mode");
        klog("PIT IRQ0 routed through Local APIC virtual-wire compatibility mode");
    }

    trace("first hardware PIT IRQ0 received");
    klog_enable_uptime();
    klog("PIT IRQ0 active; uptime clock running");
    klog_heartbeat_enable();

    /* TPM CRB command timeouts use the PIT clock. Establish the hardware trust
     * anchor before normal device use or future user processes begin. */
    initialize_tpm_security();

#if TWILIGHT_SCROLL_SELF_TEST
    framebuffer_scroll_self_test();
#endif

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

    for (;;) {
        __asm__ volatile ("hlt");
        klog_heartbeat_update();
    }
}
