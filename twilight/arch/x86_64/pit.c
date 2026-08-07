#include <stdint.h>

#include <twilight/interrupts.h>
#include <twilight/io.h>
#include <twilight/timer.h>

#define PIT_INPUT_HZ 1193182u
#define PIT_COMMAND  0x43u
#define PIT_CHANNEL0 0x40u

static volatile uint64_t ticks;
static uint64_t frequency_hz = 1000u;

void pit_init(uint32_t requested_hz) {
    if (requested_hz == 0) requested_hz = 1000u;

    uint32_t divisor = PIT_INPUT_HZ / requested_hz;
    if (divisor < 1u) divisor = 1u;
    if (divisor > 65535u) divisor = 65535u;

    frequency_hz = PIT_INPUT_HZ / divisor;
    ticks = 0;

    outb(PIT_COMMAND, 0x36u);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xffu));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xffu));
}

void pit_irq_handler(void) {
    ++ticks;
    pic_send_eoi(0);
}

uint64_t timer_ticks(void) {
    return ticks;
}

uint64_t timer_frequency(void) {
    return frequency_hz;
}

uint64_t timer_uptime_us(void) {
    const uint64_t hz = frequency_hz;
    if (hz == 0) return 0;
    return (ticks * 1000000ull) / hz;
}
