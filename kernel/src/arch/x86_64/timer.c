#include <stdbool.h>
#include <vexa/arch.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include "lapic.h"

#define PIT_FREQUENCY 1193182
#define PIT_CHANNEL2 0x42
#define PIT_COMMAND 0x43
#define PIT_GATE_PORT 0x61
#define CALIBRATION_MS 10

static volatile uint64_t ticks;

static void timer_tick(struct interrupt_frame *frame) {
    (void)frame;
    ticks++;
}

/* Counts down PIT channel 2 for `ms` milliseconds, busy-waiting until it ends.
 * Returns false if the PIT never finished (some newer machines lack one). */
static bool pit_wait(uint32_t ms, void (*on_start)(void)) {
    uint32_t count = PIT_FREQUENCY * ms / 1000;
    uint8_t gate = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, (gate & ~0x02) | 0x01); /* Speaker off, gate on. */
    outb(PIT_COMMAND, 0xb0); /* Channel 2, low then high byte, mode 0. */
    outb(PIT_CHANNEL2, count & 0xff);
    outb(PIT_CHANNEL2, count >> 8);

    gate = inb(PIT_GATE_PORT) & ~0x01;
    outb(PIT_GATE_PORT, gate); /* Restart the count with a low-to-high gate edge. */
    on_start();
    outb(PIT_GATE_PORT, gate | 0x01);

    for (uint64_t spins = 0; spins < 10000000; spins++) { /* Roughly 10 s. */
        if (inb(PIT_GATE_PORT) & 0x20) {
            return true;
        }
    }
    return false;
}

static void lapic_timer_start_calibration(void) {
    lapic_write(LAPIC_TIMER_INITIAL, 0xffffffff);
}

void timer_init(void) {
    lapic_write(LAPIC_TIMER_DIVIDE, 0x3); /* Divide the bus clock by 16. */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);

    if (!pit_wait(CALIBRATION_MS, lapic_timer_start_calibration)) {
        panic("timer: PIT did not respond; HPET calibration is not implemented yet");
    }
    uint32_t elapsed = 0xffffffff - lapic_read(LAPIC_TIMER_CURRENT);
    lapic_write(LAPIC_TIMER_INITIAL, 0);
    uint32_t per_ms = elapsed / CALIBRATION_MS;
    if (per_ms == 0) {
        panic("timer: local APIC timer did not count during calibration");
    }

    irq_register(VECTOR_TIMER, timer_tick);
    lapic_write(LAPIC_LVT_TIMER, VECTOR_TIMER | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INITIAL, per_ms);
    kprintf("[timer] local APIC timer: %u ticks/ms (bus ~%u MHz), 1000 Hz\n",
            per_ms, per_ms * 16 / 1000);
}

uint64_t timer_ms(void) {
    return ticks;
}

void timer_sleep_ms(uint64_t ms) {
    uint64_t until = ticks + ms;
    while (ticks < until) {
        cpu_wait_for_interrupt();
    }
}
