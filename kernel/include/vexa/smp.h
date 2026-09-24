#ifndef VEXA_SMP_H
#define VEXA_SMP_H

#include <limine.h>

/* Starts every other CPU the bootloader found, and waits until each runs on
 * Vexa's page tables and stacks (so bootloader memory can be reclaimed). Does
 * nothing without an APIC or with the `nosmp` option. */
void smp_start(struct limine_mp_response *mp);

/* Halts every other CPU. Used by panic(). */
void smp_stop_other_cpus(void);

#endif
