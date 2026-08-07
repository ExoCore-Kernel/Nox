#pragma once

#include <stdint.h>

void idt_init(void);
void pic_init(void);
void pic_send_eoi(uint8_t irq);
void interrupts_enable(void);
void interrupts_disable(void);
