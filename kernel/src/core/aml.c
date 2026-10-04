#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <vexa/acpi.h>
#include <vexa/arch.h>
#include <vexa/cmdline.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/pci.h>
#include <vexa/power.h>
#include <vexa/sched.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>

/*
 * ACPI's namespace, through uACPI (third_party/uacpi): the DSDT's and SSDTs'
 * AML is loaded and initialized at boot, so that PCI interrupt routing (_PRT,
 * and the link devices it names) can be evaluated, and the power button
 * works. Below are the services uACPI asks of a kernel; then the routing.
 *
 * `noaml` on the kernel command line skips all of it (and `acpi=off`, which
 * safe mode uses, leaves no tables to load).
 */

static bool namespace_ready;

/* ---- Tables and memory ---- */

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    *out = acpi_rsdp();
    return *out ? UACPI_STATUS_OK : UACPI_STATUS_NOT_FOUND;
}

void *uacpi_kernel_map(uacpi_phys_addr address, uacpi_size length) {
    /* (Everything stays mapped, as the rest of the kernel does.) */
    return map_phys(address, length, MAP_WRITEBACK);
}

void uacpi_kernel_unmap(void *address, uacpi_size length) {
    (void)address, (void)length;
}

void *uacpi_kernel_alloc(uacpi_size size) {
    return kmalloc(size);
}

void uacpi_kernel_free(void *memory, uacpi_size size) {
    (void)size;
    kfree(memory);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *text) {
    if (level <= UACPI_LOG_WARN) {
        kprintf("[acpi] %s%s", level == UACPI_LOG_ERROR ? "error: " : "", text);
    }
}

/* ---- PCI configuration space and I/O ports ---- */

struct pci_handle {
    uacpi_pci_address address;
};

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out) {
    if (address.segment != 0) {
        return UACPI_STATUS_UNIMPLEMENTED;
    }
    struct pci_handle *handle = kmalloc(sizeof(*handle));
    if (!handle) {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    handle->address = address;
    *out = handle;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {
    kfree(handle);
}

static uacpi_status pci_read(uacpi_handle handle, uacpi_size offset, int width, uint32_t *out) {
    struct pci_handle *h = handle;
    *out = pci_config_read(h->address.bus, h->address.device, h->address.function,
                           (uint16_t)offset, width);
    return UACPI_STATUS_OK;
}

static uacpi_status pci_write(uacpi_handle handle, uacpi_size offset, int width,
                              uint32_t value) {
    struct pci_handle *h = handle;
    pci_config_write(h->address.bus, h->address.device, h->address.function, (uint16_t)offset,
                     width, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size offset, uacpi_u8 *value) {
    uint32_t v;
    uacpi_status status = pci_read(h, offset, 1, &v);
    *value = (uacpi_u8)v;
    return status;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size offset, uacpi_u16 *value) {
    uint32_t v;
    uacpi_status status = pci_read(h, offset, 2, &v);
    *value = (uacpi_u16)v;
    return status;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size offset, uacpi_u32 *value) {
    return pci_read(h, offset, 4, value);
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size offset, uacpi_u8 value) {
    return pci_write(h, offset, 1, value);
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size offset, uacpi_u16 value) {
    return pci_write(h, offset, 2, value);
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size offset, uacpi_u32 value) {
    return pci_write(h, offset, 4, value);
}

/* An I/O handle is just its base port. */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length, uacpi_handle *out) {
    (void)length;
    *out = (uacpi_handle)(uintptr_t)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    (void)handle;
}

static uint16_t port_of(uacpi_handle handle, uacpi_size offset) {
    return (uint16_t)((uintptr_t)handle + offset);
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size offset, uacpi_u8 *out) {
    *out = inb(port_of(h, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size offset, uacpi_u16 *out) {
    *out = inw(port_of(h, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size offset, uacpi_u32 *out) {
    *out = inl(port_of(h, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size offset, uacpi_u8 value) {
    outb(port_of(h, offset), value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size offset, uacpi_u16 value) {
    outw(port_of(h, offset), value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size offset, uacpi_u32 value) {
    outl(port_of(h, offset), value);
    return UACPI_STATUS_OK;
}

/* ---- Time ---- */

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    return timer_ms() * 1000000ULL;
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    /* A write to port 0x80 (the POST code) takes about a microsecond. */
    for (unsigned i = 0; i < usec; i++) {
        outb(0x80, 0);
    }
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    thread_sleep_ms(msec);
}

/* ---- Locks and events ---- */

/* uACPI's mutexes can be tried with a timeout, so they're built here from a
 * flag and a wait queue rather than being kernel mutexes. */
struct aml_mutex {
    struct spinlock lock;
    bool held;
    struct wait_queue waiters;
};

uacpi_handle uacpi_kernel_create_mutex(void) {
    return kzalloc(sizeof(struct aml_mutex));
}

void uacpi_kernel_free_mutex(uacpi_handle handle) {
    kfree(handle);
}

static bool try_take(struct aml_mutex *m) {
    uint64_t flags = spin_lock_irqsave(&m->lock);
    bool taken = !m->held;
    m->held = true;
    spin_unlock_irqrestore(&m->lock, flags);
    return taken;
}

static bool mutex_free(void *arg) {
    return !((struct aml_mutex *)arg)->held;
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    struct aml_mutex *m = handle;
    uint64_t deadline = timer_ms() + timeout;
    while (!try_take(m)) {
        if (timeout == 0 || (timeout != 0xffff && timer_ms() >= deadline)) {
            return UACPI_STATUS_TIMEOUT;
        }
        uint64_t wait = timeout == 0xffff ? 100 : deadline - timer_ms();
        wait_queue_wait_timeout(&m->waiters, mutex_free, m, wait ? wait : 1, false);
    }
    return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle handle) {
    struct aml_mutex *m = handle;
    __atomic_store_n(&m->held, false, __ATOMIC_RELEASE);
    wait_queue_wake_all(&m->waiters);
}

/* Events: a counter, waited for with a timeout, signalled from anywhere. */
struct aml_event {
    struct spinlock lock;
    uint64_t count;
    struct wait_queue waiters;
};

uacpi_handle uacpi_kernel_create_event(void) {
    return kzalloc(sizeof(struct aml_event));
}

void uacpi_kernel_free_event(uacpi_handle handle) {
    kfree(handle);
}

static bool event_set(void *arg) {
    return __atomic_load_n(&((struct aml_event *)arg)->count, __ATOMIC_ACQUIRE) != 0;
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    struct aml_event *e = handle;
    uint64_t deadline = timer_ms() + timeout;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&e->lock);
        if (e->count) {
            e->count--;
            spin_unlock_irqrestore(&e->lock, flags);
            return UACPI_TRUE;
        }
        spin_unlock_irqrestore(&e->lock, flags);
        if (timeout == 0 || (timeout != 0xffff && timer_ms() >= deadline)) {
            return UACPI_FALSE;
        }
        uint64_t wait = timeout == 0xffff ? 100 : deadline - timer_ms();
        wait_queue_wait_timeout(&e->waiters, event_set, e, wait ? wait : 1, false);
    }
}

void uacpi_kernel_signal_event(uacpi_handle handle) {
    struct aml_event *e = handle;
    uint64_t flags = spin_lock_irqsave(&e->lock);
    e->count++;
    spin_unlock_irqrestore(&e->lock, flags);
    wait_queue_wake_all(&e->waiters);
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    struct aml_event *e = handle;
    uint64_t flags = spin_lock_irqsave(&e->lock);
    e->count = 0;
    spin_unlock_irqrestore(&e->lock, flags);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return thread_current();
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    if (state & RFLAGS_IF) {
        interrupts_enable();
    }
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
    return kzalloc(sizeof(struct spinlock));
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) {
    kfree(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    return spin_lock_irqsave(handle);
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    spin_unlock_irqrestore(handle, flags);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request) {
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL) {
        kprintf("[acpi] the firmware reports a fatal error\n");
    }
    return UACPI_STATUS_OK;
}

/* ---- Interrupts (the SCI) ---- */

struct aml_interrupt {
    uacpi_interrupt_handler handler;
    uacpi_handle context;
};

static bool aml_interrupt(void *arg) {
    struct aml_interrupt *i = arg;
    return i->handler(i->context) == UACPI_INTERRUPT_HANDLED;
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,
                                                    uacpi_interrupt_handler handler,
                                                    uacpi_handle context,
                                                    uacpi_handle *out) {
    struct aml_interrupt *i = kmalloc(sizeof(*i));
    if (!i) {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    i->handler = handler;
    i->context = context;
    /* The SCI: an ISA IRQ number, level-triggered and active low unless the
     * MADT says otherwise. */
    bool level = true, active_low = true;
    uint32_t gsi = irq < 16 ? irq_isa_gsi((uint8_t)irq, &level, &active_low) : irq;
    if (!irq_attach_line(gsi, level, active_low, aml_interrupt, i)) {
        kfree(i);
        return UACPI_STATUS_INTERNAL_ERROR;
    }
    *out = i;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                                      uacpi_handle handle) {
    (void)handler, (void)handle;
    return UACPI_STATUS_UNIMPLEMENTED; /* (Never needed: ACPI stays on.) */
}

/* ---- Deferred work (GPE methods, notifications): a thread each ---- */

struct aml_work {
    uacpi_work_handler handler;
    uacpi_handle context;
};

static volatile int work_running;
static struct wait_queue work_done = WAIT_QUEUE_INIT;

static void work_thread(void *arg) {
    struct aml_work *work = arg;
    work->handler(work->context);
    kfree(work);
    __atomic_fetch_sub(&work_running, 1, __ATOMIC_RELEASE);
    wait_queue_wake_all(&work_done);
    thread_exit();
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler,
                                        uacpi_handle context) {
    (void)type;
    struct aml_work *work = kmalloc(sizeof(*work));
    if (!work) {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    work->handler = handler;
    work->context = context;
    __atomic_fetch_add(&work_running, 1, __ATOMIC_ACQUIRE);
    if (!thread_create("acpi", work_thread, work)) {
        __atomic_fetch_sub(&work_running, 1, __ATOMIC_RELEASE);
        kfree(work);
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    return UACPI_STATUS_OK;
}

static bool no_work(void *arg) {
    (void)arg;
    return __atomic_load_n(&work_running, __ATOMIC_ACQUIRE) == 0;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    while (!no_work(NULL)) {
        wait_queue_wait_timeout(&work_done, no_work, NULL, 100, false);
    }
    return UACPI_STATUS_OK;
}

/* ---- Bringing the namespace up ---- */

static void power_button_work(uacpi_handle context) {
    (void)context;
    kprintf("[acpi] power button: shutting down\n");
    power_off();
}

static uacpi_interrupt_ret power_button(uacpi_handle context) {
    (void)context;
    uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION, power_button_work, NULL);
    return UACPI_INTERRUPT_HANDLED;
}

void acpi_namespace_init(void) {
    if (!acpi_rsdp() || cmdline_has("noaml")) {
        return;
    }
    uint64_t start = timer_ms();
    uacpi_status status = uacpi_initialize(0);
    if (status == UACPI_STATUS_OK) {
        status = uacpi_namespace_load();
    }
    if (status == UACPI_STATUS_OK) {
        /* \_PIC(1): tell the firmware we route interrupts through I/O APICs. */
        uacpi_set_interrupt_model(interrupt_controller_is_apic() ? UACPI_INTERRUPT_MODEL_IOAPIC
                                                                 : UACPI_INTERRUPT_MODEL_PIC);
        status = uacpi_namespace_initialize();
    }
    if (status != UACPI_STATUS_OK) {
        kprintf("[acpi] the AML interpreter couldn't start: %s\n", uacpi_status_to_string(status));
        return;
    }
    namespace_ready = true;
    uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, power_button, NULL);
    uacpi_finalize_gpe_initialization();
    kprintf("[acpi] namespace loaded and initialized (%lu ms)\n", timer_ms() - start);
}

/* ---- PCI interrupt routing ---- */

/* The host bridge's node (bus 0). */
static uacpi_iteration_decision found_root(void *user, uacpi_namespace_node *node,
                                           uacpi_u32 depth) {
    (void)depth;
    uacpi_u64 bus = 0;
    uacpi_eval_simple_integer(node, "_BBN", &bus); /* (Absent means bus 0.) */
    if (bus == 0) {
        *(uacpi_namespace_node **)user = node;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_namespace_node *root_bridge(void) {
    static uacpi_namespace_node *root;
    static const uacpi_char *const ids[] = {"PNP0A03", "PNP0A08", NULL};
    if (!root) {
        uacpi_find_devices_at(uacpi_namespace_root(), ids, found_root, &root);
    }
    return root;
}

struct child_search {
    uacpi_u64 address;
    uacpi_namespace_node *found;
};

static uacpi_iteration_decision check_child(void *user, uacpi_namespace_node *node,
                                            uacpi_u32 depth) {
    (void)depth;
    struct child_search *search = user;
    uacpi_u64 address;
    if (uacpi_eval_simple_integer(node, "_ADR", &address) == UACPI_STATUS_OK &&
        address == search->address) {
        search->found = node;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* A PCI device's node under `parent` (by its _ADR: slot << 16 | function). */
static uacpi_namespace_node *child_for(uacpi_namespace_node *parent, struct pci_device *d) {
    struct child_search search = {(uacpi_u64)d->slot << 16 | d->function, NULL};
    uacpi_namespace_for_each_child(parent, check_child, NULL, UACPI_OBJECT_DEVICE_BIT, 1,
                                   &search);
    return search.found;
}

struct link_irq {
    bool found, level, active_low;
    uint32_t irq;
};

static uacpi_iteration_decision link_resource(void *user, uacpi_resource *resource) {
    struct link_irq *link = user;
    if (resource->type == UACPI_RESOURCE_TYPE_IRQ && resource->irq.num_irqs) {
        link->irq = resource->irq.irqs[0];
        link->level = resource->irq.triggering == UACPI_TRIGGERING_LEVEL;
        link->active_low = resource->irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
        link->found = true;
    } else if (resource->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ &&
               resource->extended_irq.num_irqs) {
        link->irq = resource->extended_irq.irqs[0];
        link->level = resource->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
        link->active_low = resource->extended_irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
        link->found = true;
    } else {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    return UACPI_ITERATION_DECISION_BREAK;
}

/* Looks up (slot, pin) in the _PRT under `node`. */
static bool look_up(uacpi_namespace_node *node, int slot, int pin, uint32_t *gsi, bool *level,
                    bool *active_low) {
    uacpi_pci_routing_table *table;
    if (uacpi_get_pci_routing_table(node, &table) != UACPI_STATUS_OK) {
        return false;
    }
    bool found = false;
    for (uacpi_size i = 0; i < table->num_entries && !found; i++) {
        uacpi_pci_routing_table_entry *e = &table->entries[i];
        if ((int)(e->address >> 16) != slot || e->pin != pin) {
            continue;
        }
        if (!e->source) {
            *gsi = e->index; /* Wired straight to an I/O APIC input. */
            *level = *active_low = true;
            found = true;
        } else {
            /* Through a link device: its current setting. */
            struct link_irq link = {0};
            uacpi_for_each_device_resource(e->source, "_CRS", link_resource, &link);
            if (link.found && link.irq) {
                *gsi = link.irq;
                *level = link.level;
                *active_low = link.active_low;
                found = true;
            }
        }
    }
    uacpi_free_pci_routing_table(table);
    return found;
}

bool acpi_pci_route(struct pci_device *d, int pin, uint32_t *gsi, bool *level,
                    bool *active_low) {
    if (!namespace_ready) {
        return false;
    }
    uacpi_namespace_node *root = root_bridge();
    if (!root) {
        return false;
    }
    /* The bridges between bus 0 and the device, from the top. */
    struct pci_device *chain[16];
    int depth = 0;
    for (struct pci_device *b = d->bridge; b && depth < 16; b = b->bridge) {
        chain[depth++] = b;
    }
    /* The deepest bridge that has its own routing table is where to look;
     * below it, each bridge rotates the pins by slot (the standard swizzle). */
    uacpi_namespace_node *node = root, *table_node = root;
    int table_level = 0; /* How many bridges down table_node is. */
    for (int i = depth - 1; i >= 0 && node; i--) {
        node = child_for(node, chain[i]);
        uacpi_namespace_node *prt = NULL;
        if (node && uacpi_namespace_node_find(node, "_PRT", &prt) == UACPI_STATUS_OK && prt) {
            table_node = node;
            table_level = depth - i;
        }
    }
    /* Swizzle from the device up to the bus table_node serves. */
    int p = pin - 1;
    struct pci_device *at = d;
    for (int i = 0; i < depth - table_level; i++) {
        p = (p + at->slot) % 4;
        at = at->bridge;
    }
    return look_up(table_node, at->slot, p, gsi, level, active_low);
}
