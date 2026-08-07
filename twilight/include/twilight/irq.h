#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TWILIGHT_IRQ_SHARED (1u << 0)

typedef bool (*twilight_irq_handler_t)(uint8_t irq, void *context);

bool irq_core_init(void);
bool irq_core_is_initialized(void);
bool irq_request(uint8_t irq,
                 twilight_irq_handler_t handler,
                 void *context,
                 uint32_t flags,
                 const char *name);
bool irq_free(uint8_t irq, twilight_irq_handler_t handler, void *context);

/* Called by x86_64 assembly stubs for legacy PIC IRQ2..IRQ15. */
void legacy_irq_dispatch(uint64_t irq);
