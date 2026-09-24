#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <vexa/arch.h>
#include <vexa/fb.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/serial.h>

#define VEXA_VERSION "0.0.1"

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

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static const char *memmap_type_name(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE: return "usable";
    case LIMINE_MEMMAP_RESERVED: return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE: return "ACPI reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS: return "ACPI NVS";
    case LIMINE_MEMMAP_BAD_MEMORY: return "bad memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel and modules";
    case LIMINE_MEMMAP_FRAMEBUFFER: return "framebuffer";
    default: return "unknown";
    }
}

static void print_memory_map(void) {
    struct limine_memmap_response *memmap = memmap_request.response;
    if (!memmap) {
        panic("bootloader did not provide a memory map");
    }
    uint64_t usable = 0;
    kprintf("[mem] %lu memory map entries:\n", memmap->entry_count);
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        kprintf("  %p - %p  %s\n", (void *)e->base, (void *)(e->base + e->length),
                memmap_type_name(e->type));
        if (e->type == LIMINE_MEMMAP_USABLE) {
            usable += e->length;
        }
    }
    kprintf("[mem] %lu MiB usable\n", usable / (1024 * 1024));
}

void kmain(void) {
    serial_init();
    kprintf("\nVexa " VEXA_VERSION " booting\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        panic("bootloader does not support Limine base revision 3");
    }

    gdt_init();
    kprintf("[cpu] GDT loaded\n");
    idt_init();
    kprintf("[cpu] IDT loaded\n");
    __asm__ volatile("int3");

    if (hhdm_request.response) {
        kprintf("[mem] higher-half direct map at %p\n", (void *)hhdm_request.response->offset);
    }
    print_memory_map();

    struct limine_framebuffer_response *fbr = framebuffer_request.response;
    if (fbr && fbr->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fbr->framebuffers[0];
        kprintf("[fb] %lux%lu, %u bpp\n", fb->width, fb->height, fb->bpp);
        fb_init(fb);
        fb_draw_splash();
    } else {
        kprintf("[fb] no framebuffer available\n");
    }

    kprintf("Vexa kernel initialized. Halting.\n");
    cpu_halt_forever();
}
