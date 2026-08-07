#include <stddef.h>
#include <stdint.h>

#include <twilight/font.h>
#include <twilight/framebuffer.h>
#include <twilight/panic.h>
#include <twilight/serial.h>

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

    if (vector < 32) return names[vector];
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

static uint64_t read_cr2(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void serial_field(const char *name, uint64_t value) {
    char hex[19];
    u64_to_hex(value, hex);
    serial_write("[panic] ");
    serial_write(name);
    serial_write(": ");
    serial_write(hex);
    serial_write("\n");
}

static void serial_page_fault_bits(uint64_t error_code) {
    serial_write("[panic] page-fault decode: ");
    serial_write((error_code & (1ull << 0)) ? "protection-violation" : "not-present");
    serial_write((error_code & (1ull << 1)) ? " write" : " read");
    serial_write((error_code & (1ull << 2)) ? " user" : " supervisor");
    if ((error_code & (1ull << 3)) != 0) serial_write(" reserved-bit");
    if ((error_code & (1ull << 4)) != 0) serial_write(" instruction-fetch");
    if ((error_code & (1ull << 5)) != 0) serial_write(" protection-key");
    if ((error_code & (1ull << 6)) != 0) serial_write(" shadow-stack");
    if ((error_code & (1ull << 15)) != 0) serial_write(" sgx");
    serial_write("\n");
}

static __attribute__((noreturn)) void panic_halt(void) {
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

__attribute__((noreturn)) void kernel_panic(const char *reason) {
    const char *message = reason ? reason : "Unknown panic";

    serial_write("\n[panic] TWILIGHT KERNEL PANIC\n");
    serial_write("[panic] reason: ");
    serial_write(message);
    serial_write("\n[panic] system halted\n");

    framebuffer_clear(80, 0, 0);
    font_draw_string("TWILIGHT KERNEL PANIC", 48, 48, 255, 255, 255);
    font_draw_string(message, 48, 80, 255, 230, 230);
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

    serial_write("\n[panic] TWILIGHT CPU EXCEPTION\n");
    serial_write("[panic] exception: ");
    serial_write(exception_name(vector));
    serial_write("\n");
    serial_field("vector", vector);
    serial_field("error code", error_code);
    serial_field("RIP", rip);

    uint64_t cr2 = 0;
    if (vector == 14) {
        cr2 = read_cr2();
        serial_field("CR2/fault address", cr2);
        serial_page_fault_bits(error_code);
    }
    serial_write("[panic] system halted\n");

    framebuffer_clear(80, 0, 0);
    font_draw_string("TWILIGHT KERNEL PANIC", 48, 48, 255, 255, 255);
    font_draw_string(exception_name(vector), 48, 80, 255, 220, 220);

    font_draw_string("Vector:", 48, 120, 255, 255, 255);
    font_draw_string(vector_hex, 176, 120, 255, 255, 255);
    font_draw_string("Error code:", 48, 144, 255, 255, 255);
    font_draw_string(error_hex, 176, 144, 255, 255, 255);
    font_draw_string("RIP:", 48, 168, 255, 255, 255);
    font_draw_string(rip_hex, 176, 168, 255, 255, 255);

    if (vector == 14) {
        char cr2_hex[19];
        u64_to_hex(cr2, cr2_hex);
        font_draw_string("CR2:", 48, 192, 255, 255, 255);
        font_draw_string(cr2_hex, 176, 192, 255, 255, 255);
    }

    font_draw_string("The system has been halted.", 48, 240, 255, 255, 255);
    panic_halt();
}
