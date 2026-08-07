#pragma once

#include <stdbool.h>
#include <stdint.h>

void idt_init(void);
void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);
uint8_t pic_master_mask(void);
uint8_t pic_master_irr(void);
uint8_t pic_master_isr(void);
void interrupts_enable(void);
void interrupts_disable(void);
bool interrupts_are_enabled(void);
