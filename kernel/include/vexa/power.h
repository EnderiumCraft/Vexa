#ifndef VEXA_POWER_H
#define VEXA_POWER_H

/* Restarting and turning off the machine. Disks need nothing first: the
 * block cache writes through. Neither returns. */
__attribute__((noreturn)) void power_restart(void);
__attribute__((noreturn)) void power_off(void);

#endif
