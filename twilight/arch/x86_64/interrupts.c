#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/apic.h>
#include <twilight/interrupts.h>
#include <twilight/io.h>
#include <twilight/ioapic.h>

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
extern void irq2_stub(void);
extern void irq3_stub(void);
extern void irq4_stub(void);
extern void irq5_stub(void);
extern void irq6_stub(void);
extern void irq7_stub(void);
extern void irq8_stub(void);
extern void irq9_stub(void);
extern void irq10_stub(void);
extern void irq11_stub(void);
extern void irq12_stub(void);
extern void irq13_stub(void);
extern void irq14_stub(void);
extern void irq15_stub(void);
extern void int80_stub(void);
extern void *exception_stub_table[32];

static void idt_set_gate_dpl(uint8_t vector, void (*handler)(void), uint8_t dpl) {
    const uint64_t address = (uint64_t)handler;
    idt[vector].offset_low = (uint16_t)(address & 0xffffu);
    idt[vector].selector = kernel_cs;
    idt[vector].ist = 0;
    idt[vector].type_attr = (uint8_t)(0x8eu | ((dpl & 3u) << 5));
    idt[vector].offset_mid = (uint16_t)((address >> 16) & 0xffffu);
    idt[vector].offset_high = (uint32_t)(address >> 32);
    idt[vector].zero = 0;
}

static void idt_set_gate(uint8_t vector, void (*handler)(void)) {
    idt_set_gate_dpl(vector, handler, 0);
}

void idt_init(void) {
    __asm__ volatile ("mov %%cs, %0" : "=r"(kernel_cs));

    for (size_t i = 0; i < 256; ++i) idt_set_gate((uint8_t)i, default_interrupt_stub);
    for (size_t i = 0; i < 32; ++i) {
        idt_set_gate((uint8_t)i, (void (*)(void))exception_stub_table[i]);
    }

    void (*const legacy_irq_stubs[16])(void) = {
        irq0_stub, irq1_stub, irq2_stub, irq3_stub,
        irq4_stub, irq5_stub, irq6_stub, irq7_stub,
        irq8_stub, irq9_stub, irq10_stub, irq11_stub,
        irq12_stub, irq13_stub, irq14_stub, irq15_stub,
    };
    for (size_t irq = 0; irq < 16; ++irq) {
        idt_set_gate((uint8_t)(0x20u + irq), legacy_irq_stubs[irq]);
    }

    idt_set_gate_dpl(0x80, int80_stub, 3);

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

    outb(0x21, 0xfeu);
    outb(0xa1, 0xffu);
}

void pic_mask_irq(uint8_t irq) {
    if (irq >= 16u) return;
    if (ioapic_is_active()) {
        (void)ioapic_mask_legacy_irq(irq);
        return;
    }

    const uint16_t port = irq < 8u ? 0x21u : 0xa1u;
    const uint8_t bit = (uint8_t)(irq & 7u);
    const uint8_t mask = inb(port);
    outb(port, (uint8_t)(mask | (uint8_t)(1u << bit)));
}

void pic_unmask_irq(uint8_t irq) {
    if (irq >= 16u) return;
    if (ioapic_is_active()) {
        (void)ioapic_unmask_legacy_irq(irq);
        return;
    }

    if (irq >= 8u) {
        const uint8_t master_mask = inb(0x21u);
        outb(0x21u, (uint8_t)(master_mask & (uint8_t)~(1u << 2)));
    }

    const uint16_t port = irq < 8u ? 0x21u : 0xa1u;
    const uint8_t bit = (uint8_t)(irq & 7u);
    const uint8_t mask = inb(port);
    outb(port, (uint8_t)(mask & (uint8_t)~(1u << bit)));
}

uint8_t pic_master_mask(void) {
    return inb(0x21u);
}

uint8_t pic_master_irr(void) {
    outb(0x20u, 0x0au);
    io_wait();
    return inb(0x20u);
}

uint8_t pic_master_isr(void) {
    outb(0x20u, 0x0bu);
    io_wait();
    return inb(0x20u);
}

void pic_send_eoi(uint8_t irq) {
    if (ioapic_is_active()) {
        apic_eoi_if_needed();
        return;
    }

    if (irq >= 8) outb(0xa0, 0x20);
    outb(0x20, 0x20);
    apic_eoi_if_needed();
}

void interrupts_enable(void) {
    __asm__ volatile ("sti" ::: "memory");
}

void interrupts_disable(void) {
    __asm__ volatile ("cli" ::: "memory");
}

bool interrupts_are_enabled(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ull << 9)) != 0;
}
