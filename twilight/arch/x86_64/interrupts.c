#include <stddef.h>
#include <stdint.h>

#include <twilight/interrupts.h>
#include <twilight/io.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static uint16_t kernel_cs;

extern void default_interrupt_stub(void);
extern void irq0_stub(void);
extern void irq1_stub(void);
extern void *exception_stub_table[32];

static void idt_set_gate(uint8_t vector, void (*handler)(void)) {
    const uint64_t address = (uint64_t)handler;
    idt[vector].offset_low = (uint16_t)(address & 0xffffu);
    idt[vector].selector = kernel_cs;
    idt[vector].ist = 0;
    idt[vector].type_attr = 0x8e;
    idt[vector].offset_mid = (uint16_t)((address >> 16) & 0xffffu);
    idt[vector].offset_high = (uint32_t)(address >> 32);
    idt[vector].zero = 0;
}

void idt_init(void) {
    __asm__ volatile ("mov %%cs, %0" : "=r"(kernel_cs));

    for (size_t i = 0; i < 256; ++i) {
        idt_set_gate((uint8_t)i, default_interrupt_stub);
    }

    for (size_t i = 0; i < 32; ++i) {
        idt_set_gate((uint8_t)i, (void (*)(void))exception_stub_table[i]);
    }

    idt_set_gate(0x20, irq0_stub);
    idt_set_gate(0x21, irq1_stub);

    const struct idtr descriptor = {
        .limit = (uint16_t)(sizeof(idt) - 1),
        .base = (uint64_t)idt,
    };
    __asm__ volatile ("lidt %0" : : "m"(descriptor));
}

void pic_init(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xa0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xa1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xa1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xa1, 0x01); io_wait();

    outb(0x21, 0xfeu); /* IRQ0 only; IRQ1 stays masked until PS/2 init succeeds */
    outb(0xa1, 0xffu);
}

void pic_mask_irq(uint8_t irq) {
    const uint16_t port = irq < 8u ? 0x21u : 0xa1u;
    const uint8_t bit = (uint8_t)(irq & 7u);
    const uint8_t mask = inb(port);
    outb(port, (uint8_t)(mask | (uint8_t)(1u << bit)));
}

void pic_unmask_irq(uint8_t irq) {
    const uint16_t port = irq < 8u ? 0x21u : 0xa1u;
    const uint8_t bit = (uint8_t)(irq & 7u);
    const uint8_t mask = inb(port);
    outb(port, (uint8_t)(mask & (uint8_t)~(1u << bit)));
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(0xa0, 0x20);
    }
    outb(0x20, 0x20);
}

void interrupts_enable(void) {
    __asm__ volatile ("sti");
}

void interrupts_disable(void) {
    __asm__ volatile ("cli");
}
