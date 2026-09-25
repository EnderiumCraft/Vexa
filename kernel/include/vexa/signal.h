#ifndef VEXA_SIGNAL_H
#define VEXA_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Signals: asynchronous notices to a process (Ctrl+C, a child exiting, a
 * fault...). Numbers are VX_SIG* from abi/vexa/abi.h, the usual Unix ones.
 *
 * Each process chooses, per signal, to take the default action, ignore it, or
 * (Linux programs) run a handler. Pending signals are acted on just before the
 * process returns to user mode; blocking waits end early with -VX_EINTR so
 * that happens promptly.
 */

struct process;
struct thread;
struct interrupt_frame;

enum signal_action {
    SIGNAL_DEFAULT,
    SIGNAL_IGNORE,
    SIGNAL_HANDLER, /* Set up by the personality (Linux sigaction). */
};

void signal_send(struct process *process, int signal);
/* The same, for callers holding the scheduler lock. */
void signal_send_locked(struct process *process, int signal);
/* A signal for one thread (Linux tgkill). Call with the scheduler lock held. */
void signal_send_thread_locked(struct thread *thread, int signal);
/* Sends to every process in a process group; returns how many got it. */
int signal_send_group(uint32_t group, int signal);
/* True if the thread has a signal it doesn't block. Safe under the scheduler lock. */
bool thread_signal_pending(struct thread *thread);
/* On the way back to user mode: acts on pending signals. May not return. */
void signal_deliver(struct interrupt_frame *frame);
/* A fault in user mode (e.g. SIGSEGV): runs the handler if there is one,
 * otherwise ends the process. */
void signal_fault(struct interrupt_frame *frame, int signal, const char *reason);

#endif
