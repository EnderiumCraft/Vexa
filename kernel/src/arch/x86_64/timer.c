#include <stdbool.h>
#include <vexa/arch.h>
#include <vexa/cpu.h>
#include <vexa/sched.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include "irqchip.h"

#define PIT_FREQUENCY 1193182
#define PIT_CHANNEL0 0x40
#define PIT_CHANNEL2 0x42
#define PIT_COMMAND 0x43
#define PIT_GATE_PORT 0x61
#define CALIBRATION_MS 10

static volatile uint64_t ticks;
static uint32_t lapic_ticks_per_ms;

/* Runs on every CPU's timer. Only the bootstrap CPU keeps the time. */
static void timer_tick(struct interrupt_frame *frame) {
    (void)frame;
    if (cpu_current()->id == 0) {
        ticks++;
    }
    sched_tick();
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

/* Without an APIC, the PIT itself generates the ticks on ISA IRQ 0. */
static void pit_timer_init(void) {
    uint16_t divisor = PIT_FREQUENCY / 1000;
    outb(PIT_COMMAND, 0x34); /* Channel 0, low then high byte, rate generator. */
    outb(PIT_CHANNEL0, divisor & 0xff);
    outb(PIT_CHANNEL0, divisor >> 8);
    isa_irq_enable(0, timer_tick);
    kprintf("[timer] PIT at 1000 Hz\n");
}

static void lapic_timer_init(void) {
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

    lapic_ticks_per_ms = per_ms;
    irq_register(VECTOR_APIC_TIMER, timer_tick);
    lapic_write(LAPIC_LVT_TIMER, VECTOR_APIC_TIMER | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INITIAL, per_ms);
    kprintf("[timer] local APIC timer: %u ticks/ms (bus ~%u MHz), 1000 Hz\n",
            per_ms, per_ms * 16 / 1000);
}

void timer_init(void) {
    if (interrupt_controller_is_apic()) {
        lapic_timer_init();
    } else {
        pit_timer_init();
    }
}

void timer_init_ap(void) {
    /* Every core's bus clock matches the bootstrap CPU's, so reuse its calibration. */
    lapic_write(LAPIC_TIMER_DIVIDE, 0x3);
    lapic_write(LAPIC_LVT_TIMER, VECTOR_APIC_TIMER | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INITIAL, lapic_ticks_per_ms);
}

uint64_t timer_ms(void) {
    return ticks;
}

void timer_sleep_ms(uint64_t ms) {
    struct thread *thread = thread_current();
    if (thread && thread != cpu_current()->idle) {
        thread_sleep_ms(ms);
        return;
    }
    uint64_t until = ticks + ms;
    while (ticks < until) {
        cpu_wait_for_interrupt();
    }
}
