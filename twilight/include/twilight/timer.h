#pragma once

#include <stdint.h>

void pit_init(uint32_t frequency_hz);
void pit_irq_handler(void);
uint64_t timer_ticks(void);
uint64_t timer_frequency(void);
uint64_t timer_uptime_us(void);
