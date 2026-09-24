#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <vexa/acpi.h>
#include <vexa/arch.h>
#include <vexa/cmdline.h>
#include <vexa/cpu.h>
#include <vexa/console.h>
#include <vexa/fb.h>
#include <vexa/fpu.h>
#include <vexa/keyboard.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/monitor.h>
#include <vexa/programs.h>
#include <vexa/sched.h>
#include <vexa/serial.h>
#include <vexa/smp.h>
#include <vexa/string.h>
#include <vexa/version.h>


/* Limine boot protocol requests. The bootloader scans for these and fills in
 * the response pointers before jumping to kmain. */
__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .flags = 0, /* Leave the local APICs in xAPIC mode where possible. */
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

#define MAX_MEMORY_RANGES 128
#define KERNEL_STACK_SIZE (64 * 1024)

/* Copied out of the bootloader's responses, which live in memory that is
 * reclaimed during boot. */
static struct mem_range memory_ranges[MAX_MEMORY_RANGES];
static size_t memory_range_count;
static uint64_t rsdp_phys;

static void console_setup(void) {
    struct limine_framebuffer_response *fbr = framebuffer_request.response;
    if (!fbr || fbr->framebuffer_count == 0) {
        kprintf("[fb] no framebuffer available; serial console only\n");
        return;
    }
    struct limine_framebuffer *fb = fbr->framebuffers[0];
    if (!fb_init(fb)) {
        kprintf("[fb] unsupported %u bpp framebuffer; serial console only\n", fb->bpp);
        return;
    }
    console_init();
    console_set_color(CONSOLE_COLOR_ACCENT);
    kprintf("Vexa " VEXA_VERSION "\n");
    console_reset_color();
    kprintf("[fb] %lux%lu, %u bpp\n", fb->width, fb->height, fb->bpp);
}

static void copy_boot_info(void) {
    struct limine_memmap_response *memmap = memmap_request.response;
    if (!memmap || !hhdm_request.response || !executable_address_request.response) {
        panic("bootloader did not provide a memory map, direct map and kernel address");
    }
    if (memmap->entry_count > MAX_MEMORY_RANGES) {
        kprintf("[mem] warning: using the first %d of %lu memory map entries\n",
                MAX_MEMORY_RANGES, memmap->entry_count);
    }
    for (uint64_t i = 0; i < memmap->entry_count && i < MAX_MEMORY_RANGES; i++) {
        memory_ranges[i] = (struct mem_range){
            .base = memmap->entries[i]->base,
            .length = memmap->entries[i]->length,
            .type = memmap->entries[i]->type,
        };
        memory_range_count++;
    }
    hhdm_offset = hhdm_request.response->offset;
    if (rsdp_request.response) {
        rsdp_phys = (uint64_t)rsdp_request.response->address;
    }
    if (cmdline_request.response) {
        cmdline_init(cmdline_request.response->cmdline);
    }
}

/* The rest of boot, running on a kernel stack with a guard page. It ends as
 * the bootstrap CPU's idle thread. */
__attribute__((noreturn)) static void kmain_on_kernel_stack(void) {
    acpi_init(rsdp_phys);
    interrupt_controller_init();
    timer_init();
    sched_init_cpu(&cpus[0]);
    interrupts_enable();
    kprintf("[cpu] vector registers: %s\n", fpu_describe());

    /* Other CPUs start on bootloader stacks and page tables, so start them
     * before reclaiming that memory. */
    smp_start(mp_request.response);
    programs_init(module_request.response);
    pmm_reclaim_bootloader_memory();

    keyboard_init();
    kprintf("\nVexa kernel initialized.\n");
    if (!thread_create("monitor", monitor_thread, NULL)) {
        panic("could not start the monitor");
    }
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

void kmain(void) {
    serial_init();
    kprintf("\nVexa " VEXA_VERSION " booting\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        panic("bootloader does not support Limine base revision 3");
    }
    console_setup();
    copy_boot_info();
    if (*cmdline_get()) {
        kprintf("[boot] command line: %s\n", cmdline_get());
    }

    idt_init();
    cpu_init_bsp();
    kprintf("[cpu] descriptor tables and CPU features set up\n");

    pmm_init(memory_ranges, memory_range_count);
    vmm_init(memory_ranges, memory_range_count,
             executable_address_request.response->physical_base,
             executable_address_request.response->virtual_base);

    /* Leave the bootloader's stack so its memory can be reclaimed. */
    uint64_t stack_top = vmm_alloc_kernel_stack(KERNEL_STACK_SIZE);
    __asm__ volatile(
        "mov %0, %%rsp\n"
        "xor %%ebp, %%ebp\n"
        "call *%1\n"
        :
        : "r"(stack_top), "r"(kmain_on_kernel_stack)
        : "memory");
    __builtin_unreachable();
}
