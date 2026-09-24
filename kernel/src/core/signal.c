#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/process.h>
#include <vexa/sched.h>
#include <vexa/signal.h>

#define BIT(signal) (1ULL << ((signal) - 1))

/* Signals whose default action is to do nothing. (Stopping a process, the
 * default for SIGSTOP and friends, isn't supported yet; they're ignored.) */
static const uint64_t ignored_by_default = BIT(VX_SIGCHLD) | BIT(VX_SIGCONT) | BIT(VX_SIGURG) |
                                           BIT(VX_SIGWINCH) | BIT(VX_SIGSTOP) | BIT(VX_SIGTSTP) |
                                           BIT(VX_SIGTTIN) | BIT(VX_SIGTTOU);

static bool valid(int signal) {
    return signal >= 1 && signal <= VX_SIGNAL_COUNT;
}

static bool would_ignore(struct process *process, int signal) {
    if (signal == VX_SIGKILL) {
        return false;
    }
    enum signal_action action = process->signal_actions[signal - 1];
    return action == SIGNAL_IGNORE || (action == SIGNAL_DEFAULT && (ignored_by_default & BIT(signal)));
}

static void mark_pending(struct process *process, int signal) {
    if (!valid(signal) || process->state != PROCESS_RUNNING || would_ignore(process, signal)) {
        return;
    }
    __atomic_or_fetch(&process->pending_signals, BIT(signal), __ATOMIC_SEQ_CST);
}

void signal_send(struct process *process, int signal) {
    mark_pending(process, signal);
    sched_interrupt_process(process);
}

void signal_send_locked(struct process *process, int signal) {
    mark_pending(process, signal);
    if (process->main_thread && thread_signal_pending(process->main_thread)) {
        sched_interrupt_locked(process->main_thread);
    }
}

struct group_send {
    uint32_t group;
    struct process *targets[64];
    int count;
};

static void collect_group(struct process *process, void *arg) {
    struct group_send *send = arg;
    if (process->group == send->group && process->state == PROCESS_RUNNING && send->count < 64) {
        object_ref(&process->object);
        send->targets[send->count++] = process;
    }
}

int signal_send_group(uint32_t group, int signal) {
    /* Collect first, then send: sending takes the scheduler lock, which must
     * not be taken while holding the process list lock. */
    struct group_send send = {.group = group};
    process_for_each(collect_group, &send);
    for (int i = 0; i < send.count; i++) {
        signal_send(send.targets[i], signal);
        object_put(&send.targets[i]->object);
    }
    return send.count;
}

bool thread_signal_pending(struct thread *thread) {
    struct process *process = thread->process;
    if (!process) {
        return false;
    }
    uint64_t pending = __atomic_load_n(&process->pending_signals, __ATOMIC_ACQUIRE);
    return pending & ~(thread->blocked_signals & ~BIT(VX_SIGKILL));
}

static int take_signal(struct thread *thread) {
    struct process *process = thread->process;
    for (;;) {
        uint64_t pending = __atomic_load_n(&process->pending_signals, __ATOMIC_ACQUIRE);
        uint64_t deliverable = pending & ~(thread->blocked_signals & ~BIT(VX_SIGKILL));
        if (!deliverable) {
            return 0;
        }
        int signal = __builtin_ctzll(deliverable) + 1;
        if (__atomic_compare_exchange_n(&process->pending_signals, &pending,
                                        pending & ~BIT(signal), false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
            return signal;
        }
    }
}

void signal_deliver(struct interrupt_frame *frame) {
    struct thread *thread = thread_current();
    struct process *process = thread ? thread->process : NULL;
    if (!process || !(frame->cs & 3)) {
        return;
    }
    int signal;
    while ((signal = take_signal(thread)) != 0) {
        enum signal_action action = process->signal_actions[signal - 1];
        if (signal == VX_SIGKILL) {
            action = SIGNAL_DEFAULT;
        }
        if (action == SIGNAL_IGNORE || (action == SIGNAL_DEFAULT && (ignored_by_default & BIT(signal)))) {
            continue;
        }
        if (action == SIGNAL_HANDLER && process->personality->deliver_signal &&
            process->personality->deliver_signal(frame, signal)) {
            return; /* One handler at a time; the rest wait for its return. */
        }
        process_exit_by_signal(signal);
    }
}

void signal_fault(struct interrupt_frame *frame, int signal, const char *reason) {
    struct thread *thread = thread_current();
    struct process *process = thread->process;
    bool handled = process->signal_actions[signal - 1] == SIGNAL_HANDLER &&
                   !(thread->blocked_signals & BIT(signal)) &&
                   process->personality->deliver_signal &&
                   process->personality->deliver_signal(frame, signal);
    if (!handled) {
        kprintf("[proc] %s (process %u) stopped: %s\n", process->name, process->id, reason);
        process_exit_by_signal(signal);
    }
}
