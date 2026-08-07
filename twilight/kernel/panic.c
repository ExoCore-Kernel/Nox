#include <stddef.h>
#include <stdint.h>

#include <twilight/font.h>
#include <twilight/framebuffer.h>
#include <twilight/panic.h>

static const char *exception_name(uint64_t vector) {
    static const char *names[32] = {
        "Divide Error", "Debug", "Non-maskable Interrupt", "Breakpoint",
        "Overflow", "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
        "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
        "x87 Floating-Point Exception", "Alignment Check", "Machine Check", "SIMD Floating-Point Exception",
        "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved",
        "Hypervisor Injection Exception", "VMM Communication Exception", "Security Exception", "Reserved"
    };

    if (vector < 32) {
        return names[vector];
    }
    return "Unexpected Interrupt";
}

static void u64_to_hex(uint64_t value, char out[19]) {
    static const char digits[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    for (size_t i = 0; i < 16; ++i) {
        const unsigned shift = (unsigned)((15 - i) * 4);
        out[2 + i] = digits[(value >> shift) & 0xFu];
    }
    out[18] = '\0';
}

static __attribute__((noreturn)) void panic_halt(void) {
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

__attribute__((noreturn)) void kernel_panic(const char *reason) {
    framebuffer_clear(80, 0, 0);
    font_draw_string("TWILIGHT KERNEL PANIC", 48, 48, 255, 255, 255);
    font_draw_string(reason ? reason : "Unknown panic", 48, 80, 255, 230, 230);
    font_draw_string("The system has been halted.", 48, 128, 255, 255, 255);
    panic_halt();
}

__attribute__((noreturn)) void kernel_panic_exception(uint64_t vector,
                                                      uint64_t error_code,
                                                      uint64_t rip) {
    char vector_hex[19];
    char error_hex[19];
    char rip_hex[19];
    u64_to_hex(vector, vector_hex);
    u64_to_hex(error_code, error_hex);
    u64_to_hex(rip, rip_hex);

    framebuffer_clear(80, 0, 0);
    font_draw_string("TWILIGHT KERNEL PANIC", 48, 48, 255, 255, 255);
    font_draw_string(exception_name(vector), 48, 80, 255, 220, 220);

    font_draw_string("Vector:", 48, 120, 255, 255, 255);
    font_draw_string(vector_hex, 176, 120, 255, 255, 255);
    font_draw_string("Error code:", 48, 144, 255, 255, 255);
    font_draw_string(error_hex, 176, 144, 255, 255, 255);
    font_draw_string("RIP:", 48, 168, 255, 255, 255);
    font_draw_string(rip_hex, 176, 168, 255, 255, 255);
    font_draw_string("The system has been halted.", 48, 216, 255, 255, 255);

    panic_halt();
}
