#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TWILIGHT_IRQ_SHARED (1u << 0)

/* Keep MSI vectors away from CPU exceptions, legacy IRQ vectors and the DPL3
 * int 0x80 gate. Sixty-four vectors are plenty for the current single-CPU
 * driver bring-up while leaving room for future IPIs and scheduler vectors. */
#define TWILIGHT_MSI_VECTOR_BASE  0x40u
#define TWILIGHT_MSI_VECTOR_COUNT 64u
#define TWILIGHT_MSI_VECTOR_END   (TWILIGHT_MSI_VECTOR_BASE + TWILIGHT_MSI_VECTOR_COUNT)

typedef bool (*twilight_irq_handler_t)(uint8_t irq, void *context);

bool irq_core_init(void);
bool irq_core_is_initialized(void);
bool irq_request(uint8_t irq,
                 twilight_irq_handler_t handler,
                 void *context,
                 uint32_t flags,
                 const char *name);
bool irq_free(uint8_t irq, twilight_irq_handler_t handler, void *context);

/* Allocate/free a CPU interrupt vector for PCI MSI/MSI-X. The returned value is
 * also used as the Linux-visible IRQ number while the device is in MSI mode. */
bool irq_allocate_msi_vector(uint8_t *vector_out);
void irq_release_msi_vector(uint8_t vector);
bool irq_is_msi_vector(uint8_t irq);

/* Called by x86_64 assembly stubs for legacy PIC/IOAPIC IRQ2..IRQ15. */
void legacy_irq_dispatch(uint64_t irq);

/* Called by the dedicated IDT stubs for dynamically allocated MSI vectors. */
void msi_irq_dispatch(uint64_t vector);
