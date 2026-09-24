#ifndef VEXA_MONITOR_H
#define VEXA_MONITOR_H

/* A small built-in command line for poking at the kernel until Vexa has a
 * shell (Phase 5 replaces it with vsh). Runs as a kernel thread. */
void monitor_thread(void *unused);
/* Runs one monitor command (for the `sys` program), printing to the console. */
void monitor_command(const char *line);

/* Mounts file systems and starts devices, then the monitor (core/init.c). */
void init_thread(void *unused);

#endif
