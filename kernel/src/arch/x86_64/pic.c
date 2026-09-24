#include <vexa/arch.h>
#include <vexa/io.h>
#include "irqchip.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xa0
#define PIC2_DATA 0xa1

#define PIC_EOI 0x20
#define PIC_READ_ISR 0x0b
#define PIC_CASCADE_IRQ 2

void pic_init(void) {
    outb(PIC1_COMMAND, 0x11); /* ICW1: initialize, expect ICW4. */
    outb(PIC2_COMMAND, 0x11);
    outb(PIC1_DATA, VECTOR_ISA_BASE); /* ICW2: vector offsets. */
    outb(PIC2_DATA, VECTOR_ISA_BASE + 8);
    outb(PIC1_DATA, 1 << PIC_CASCADE_IRQ); /* ICW3: slave PIC on IRQ 2. */
    outb(PIC2_DATA, PIC_CASCADE_IRQ);
    outb(PIC1_DATA, 0x01); /* ICW4: 8086 mode. */
    outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, 0xff); /* Mask everything. */
    outb(PIC2_DATA, 0xff);
}

void pic_unmask(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_DATA, inb(PIC2_DATA) & ~(1 << (irq - 8)));
        irq = PIC_CASCADE_IRQ; /* The slave's interrupts arrive through IRQ 2. */
    }
    outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << irq));
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

/* IRQ 7 and 15 can fire spuriously; the in-service register tells us if they're real. */
bool pic_is_spurious(uint8_t irq) {
    if (irq == 7) {
        outb(PIC1_COMMAND, PIC_READ_ISR);
        return !(inb(PIC1_COMMAND) & 0x80);
    }
    if (irq == 15) {
        outb(PIC2_COMMAND, PIC_READ_ISR);
        if (!(inb(PIC2_COMMAND) & 0x80)) {
            outb(PIC1_COMMAND, PIC_EOI); /* The master still saw a real cascade IRQ. */
            return true;
        }
    }
    return false;
}
