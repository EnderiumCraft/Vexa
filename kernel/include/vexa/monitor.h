#ifndef VEXA_MONITOR_H
#define VEXA_MONITOR_H

/* A small built-in command line for poking at the kernel until Vexa can run
 * user programs (Phase 5 replaces it with vsh). Never returns. */
__attribute__((noreturn)) void monitor_run(void);

#endif
