#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/io.h>
#include <twilight/pci.h>

#define PCI_CONFIG_ADDRESS 0x0cf8u
#define PCI_CONFIG_DATA    0x0cfcu

#define PCI_COMMAND_IO     (1u << 0)
#define PCI_COMMAND_MEMORY (1u << 1)
#define PCI_COMMAND_MASTER (1u << 2)

static struct twilight_pci_device devices[TWILIGHT_PCI_MAX_DEVICES];
static size_t device_count_value;
static bool initialized;
static volatile uint32_t config_lock;

static uint64_t lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&config_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void unlock_irqrestore(uint64_t flags) {
    __atomic_store_n(&config_lock, 0u, __ATOMIC_RELEASE);
    if ((flags & (1ull << 9)) != 0) __asm__ volatile ("sti" : : : "memory");
}

static uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset) {
    return 0x80000000u |
           ((uint32_t)bus << 16) |
           ((uint32_t)(slot & 0x1fu) << 11) |
           ((uint32_t)(function & 0x07u) << 8) |
           ((uint32_t)offset & 0xfcu);
}

static uint32_t raw_read32(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset) {
    outl(PCI_CONFIG_ADDRESS, config_address(bus, slot, function, offset));
    return inl(PCI_CONFIG_DATA);
}

static void raw_write32(uint8_t bus,
                        uint8_t slot,
                        uint8_t function,
                        uint16_t offset,
                        uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, config_address(bus, slot, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

static uint32_t cfg_read32_locked(uint8_t bus,
                                  uint8_t slot,
                                  uint8_t function,
                                  uint16_t offset) {
    return raw_read32(bus, slot, function, offset);
}

static uint16_t cfg_read16_locked(uint8_t bus,
                                  uint8_t slot,
                                  uint8_t function,
                                  uint16_t offset) {
    const uint32_t value = raw_read32(bus, slot, function, offset);
    return (uint16_t)(value >> ((offset & 2u) * 8u));
}

static uint8_t cfg_read8_locked(uint8_t bus,
                                uint8_t slot,
                                uint8_t function,
                                uint16_t offset) {
    const uint32_t value = raw_read32(bus, slot, function, offset);
    return (uint8_t)(value >> ((offset & 3u) * 8u));
}

static void cfg_write16_locked(uint8_t bus,
                               uint8_t slot,
                               uint8_t function,
                               uint16_t offset,
                               uint16_t value) {
    const uint16_t aligned = (uint16_t)(offset & ~3u);
    uint32_t current = raw_read32(bus, slot, function, aligned);
    const unsigned shift = (unsigned)((offset & 2u) * 8u);
    current &= ~(0xffffu << shift);
    current |= (uint32_t)value << shift;
    raw_write32(bus, slot, function, aligned, current);
}

static void probe_bar(struct twilight_pci_device *device, unsigned index) {
    if (index >= TWILIGHT_PCI_MAX_BARS) return;

    const uint16_t offset = (uint16_t)(0x10u + index * 4u);
    const uint32_t original_low = cfg_read32_locked(device->bus,
                                                     device->slot,
                                                     device->function,
                                                     offset);

    struct twilight_pci_bar *bar = &device->bars[index];
    *bar = (struct twilight_pci_bar){0};

    if ((original_low & 1u) != 0) {
        raw_write32(device->bus, device->slot, device->function, offset, 0xffffffffu);
        const uint32_t mask = cfg_read32_locked(device->bus,
                                                device->slot,
                                                device->function,
                                                offset);
        raw_write32(device->bus, device->slot, device->function, offset, original_low);

        const uint32_t size_mask = mask & ~3u;
        if (size_mask == 0u) return;

        bar->address = (uint64_t)(original_low & ~3u);
        bar->size = (uint64_t)(~size_mask + 1u);
        bar->flags = TWILIGHT_PCI_BAR_IO;
        return;
    }

    const uint32_t memory_type = (original_low >> 1) & 3u;
    const bool is_64 = memory_type == 2u;
    if (memory_type == 3u) return;

    uint32_t original_high = 0;
    if (is_64) {
        if (index + 1u >= TWILIGHT_PCI_MAX_BARS) return;
        original_high = cfg_read32_locked(device->bus,
                                          device->slot,
                                          device->function,
                                          (uint16_t)(offset + 4u));
    }

    raw_write32(device->bus, device->slot, device->function, offset, 0xffffffffu);
    if (is_64) {
        raw_write32(device->bus,
                    device->slot,
                    device->function,
                    (uint16_t)(offset + 4u),
                    0xffffffffu);
    }

    const uint32_t mask_low = cfg_read32_locked(device->bus,
                                                device->slot,
                                                device->function,
                                                offset);
    uint32_t mask_high = 0;
    if (is_64) {
        mask_high = cfg_read32_locked(device->bus,
                                      device->slot,
                                      device->function,
                                      (uint16_t)(offset + 4u));
    }

    raw_write32(device->bus, device->slot, device->function, offset, original_low);
    if (is_64) {
        raw_write32(device->bus,
                    device->slot,
                    device->function,
                    (uint16_t)(offset + 4u),
                    original_high);
    }

    if (is_64) {
        const uint64_t mask = ((uint64_t)mask_high << 32) |
                              (uint64_t)(mask_low & ~0x0fu);
        if (mask == 0) return;
        bar->address = ((uint64_t)original_high << 32) |
                       (uint64_t)(original_low & ~0x0fu);
        bar->size = ~mask + 1ull;
        bar->flags = TWILIGHT_PCI_BAR_MEM64;
    } else {
        const uint32_t size_mask = mask_low & ~0x0fu;
        if (size_mask == 0u) return;
        bar->address = (uint64_t)(original_low & ~0x0fu);
        bar->size = (uint64_t)(~size_mask + 1u);
        bar->flags = 0;
    }

    if ((original_low & (1u << 3)) != 0) bar->flags |= TWILIGHT_PCI_BAR_PREFETCH;
}

static bool enumerate_function(uint8_t bus, uint8_t slot, uint8_t function) {
    const uint16_t vendor = cfg_read16_locked(bus, slot, function, 0x00u);
    if (vendor == 0xffffu || vendor == 0x0000u) return true;
    if (device_count_value >= TWILIGHT_PCI_MAX_DEVICES) return false;

    struct twilight_pci_device *device = &devices[device_count_value];
    *device = (struct twilight_pci_device){0};

    device->vendor_id = vendor;
    device->device_id = cfg_read16_locked(bus, slot, function, 0x02u);
    device->revision = cfg_read8_locked(bus, slot, function, 0x08u);
    device->prog_if = cfg_read8_locked(bus, slot, function, 0x09u);
    device->subclass = cfg_read8_locked(bus, slot, function, 0x0au);
    device->class_code = cfg_read8_locked(bus, slot, function, 0x0bu);
    device->header_type = cfg_read8_locked(bus, slot, function, 0x0eu);
    device->bus = bus;
    device->slot = slot;
    device->function = function;
    device->irq_line = cfg_read8_locked(bus, slot, function, 0x3cu);
    device->irq_pin = cfg_read8_locked(bus, slot, function, 0x3du);

    const uint8_t header_layout = (uint8_t)(device->header_type & 0x7fu);
    if (header_layout == 0u) {
        device->subsystem_vendor_id = cfg_read16_locked(bus, slot, function, 0x2cu);
        device->subsystem_device_id = cfg_read16_locked(bus, slot, function, 0x2eu);
    }

    const uint16_t original_command = cfg_read16_locked(bus, slot, function, 0x04u);
    cfg_write16_locked(bus,
                       slot,
                       function,
                       0x04u,
                       (uint16_t)(original_command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY)));

    const unsigned bar_count = header_layout == 0u ? 6u : (header_layout == 1u ? 2u : 0u);
    for (unsigned bar = 0; bar < bar_count; ++bar) {
        probe_bar(device, bar);
        if ((device->bars[bar].flags & TWILIGHT_PCI_BAR_MEM64) != 0) ++bar;
    }

    cfg_write16_locked(bus, slot, function, 0x04u, original_command);
    ++device_count_value;
    return true;
}

bool pci_init(void) {
    initialized = false;
    device_count_value = 0;
    config_lock = 0;

    const uint64_t flags = lock_irqsave();
    bool ok = true;

    for (unsigned bus = 0; bus < 256u && ok; ++bus) {
        for (unsigned slot = 0; slot < 32u && ok; ++slot) {
            const uint16_t vendor0 = cfg_read16_locked((uint8_t)bus, (uint8_t)slot, 0u, 0x00u);
            if (vendor0 == 0xffffu || vendor0 == 0x0000u) continue;

            ok = enumerate_function((uint8_t)bus, (uint8_t)slot, 0u);
            if (!ok) break;

            const uint8_t header = cfg_read8_locked((uint8_t)bus, (uint8_t)slot, 0u, 0x0eu);
            if ((header & 0x80u) == 0) continue;

            for (unsigned function = 1; function < 8u; ++function) {
                if (!enumerate_function((uint8_t)bus,
                                        (uint8_t)slot,
                                        (uint8_t)function)) {
                    ok = false;
                    break;
                }
            }
        }
    }

    unlock_irqrestore(flags);
    if (!ok) return false;

    initialized = true;
    return true;
}

bool pci_is_initialized(void) {
    return initialized;
}

size_t pci_device_count(void) {
    return initialized ? device_count_value : 0;
}

struct twilight_pci_device *pci_device_at(size_t index) {
    if (!initialized || index >= device_count_value) return 0;
    return &devices[index];
}

uint8_t pci_config_read8(const struct twilight_pci_device *device, uint16_t offset) {
    if (device == 0) return 0xffu;
    const uint64_t flags = lock_irqsave();
    const uint8_t value = cfg_read8_locked(device->bus, device->slot, device->function, offset);
    unlock_irqrestore(flags);
    return value;
}

uint16_t pci_config_read16(const struct twilight_pci_device *device, uint16_t offset) {
    if (device == 0) return 0xffffu;
    const uint64_t flags = lock_irqsave();
    const uint16_t value = cfg_read16_locked(device->bus, device->slot, device->function, offset);
    unlock_irqrestore(flags);
    return value;
}

uint32_t pci_config_read32(const struct twilight_pci_device *device, uint16_t offset) {
    if (device == 0) return 0xffffffffu;
    const uint64_t flags = lock_irqsave();
    const uint32_t value = cfg_read32_locked(device->bus, device->slot, device->function, offset);
    unlock_irqrestore(flags);
    return value;
}

void pci_config_write8(const struct twilight_pci_device *device, uint16_t offset, uint8_t value) {
    if (device == 0) return;
    const uint64_t flags = lock_irqsave();
    const uint16_t aligned = (uint16_t)(offset & ~3u);
    uint32_t current = cfg_read32_locked(device->bus, device->slot, device->function, aligned);
    const unsigned shift = (unsigned)((offset & 3u) * 8u);
    current &= ~(0xffu << shift);
    current |= (uint32_t)value << shift;
    raw_write32(device->bus, device->slot, device->function, aligned, current);
    unlock_irqrestore(flags);
}

void pci_config_write16(const struct twilight_pci_device *device, uint16_t offset, uint16_t value) {
    if (device == 0) return;
    const uint64_t flags = lock_irqsave();
    cfg_write16_locked(device->bus, device->slot, device->function, offset, value);
    unlock_irqrestore(flags);
}

void pci_config_write32(const struct twilight_pci_device *device, uint16_t offset, uint32_t value) {
    if (device == 0) return;
    const uint64_t flags = lock_irqsave();
    raw_write32(device->bus, device->slot, device->function, offset, value);
    unlock_irqrestore(flags);
}

bool pci_enable_device_native(struct twilight_pci_device *device) {
    if (!initialized || device == 0) return false;

    uint16_t command = pci_config_read16(device, 0x04u);
    bool has_io = false;
    bool has_memory = false;

    for (unsigned i = 0; i < TWILIGHT_PCI_MAX_BARS; ++i) {
        if (device->bars[i].size == 0) continue;
        if ((device->bars[i].flags & TWILIGHT_PCI_BAR_IO) != 0) has_io = true;
        else has_memory = true;
    }

    if (has_io) command |= PCI_COMMAND_IO;
    if (has_memory) command |= PCI_COMMAND_MEMORY;
    pci_config_write16(device, 0x04u, command);
    return true;
}

void pci_set_bus_master_native(struct twilight_pci_device *device, bool enabled) {
    if (!initialized || device == 0) return;
    uint16_t command = pci_config_read16(device, 0x04u);
    if (enabled) command |= PCI_COMMAND_MASTER;
    else command &= (uint16_t)~PCI_COMMAND_MASTER;
    pci_config_write16(device, 0x04u, command);
}
