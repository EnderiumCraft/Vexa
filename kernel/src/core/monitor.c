#include <stddef.h>
#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/console.h>
#include <vexa/fb.h>
#include <vexa/io.h>
#include <vexa/keyboard.h>
#include <limine.h>
#include <vexa/kprintf.h>
#include <vexa/memtest.h>
#include <vexa/mm.h>
#include <vexa/monitor.h>
#include <vexa/string.h>

#define LINE_MAX 128

struct command {
    const char *name;
    const char *help;
    void (*run)(void);
};

static void cmd_help(void);
static void cmd_clear(void);
static void cmd_uptime(void);
static void cmd_cpu(void);
static void cmd_reboot(void);
static void cmd_mem(void);
static void cmd_memmap(void);
static void cmd_memtest(void);

static const struct command commands[] = {
    {"help", "list commands", cmd_help},
    {"clear", "clear the screen", cmd_clear},
    {"uptime", "time since boot", cmd_uptime},
    {"cpu", "processor and display information", cmd_cpu},
    {"mem", "memory usage", cmd_mem},
    {"memmap", "physical memory map from the firmware", cmd_memmap},
    {"memtest", "stress-test the memory allocators", cmd_memtest},
    {"reboot", "restart the machine", cmd_reboot},
};

static void cmd_help(void) {
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        kprintf("  %s", commands[i].name);
        for (size_t pad = strlen(commands[i].name); pad < 9; pad++) {
            kprintf(" ");
        }
        kprintf("%s\n", commands[i].help);
    }
}

static void cmd_clear(void) {
    console_clear();
}

static void cmd_uptime(void) {
    uint64_t ms = timer_ms();
    uint64_t s = ms / 1000;
    kprintf("up %lu:%s%lu:%s%lu.%s%s%lu\n", s / 3600,
            (s / 60) % 60 < 10 ? "0" : "", (s / 60) % 60,
            s % 60 < 10 ? "0" : "", s % 60,
            ms % 1000 < 100 ? "0" : "", ms % 1000 < 10 ? "0" : "", ms % 1000);
}

static void cmd_cpu(void) {
    uint32_t a, b, c, d;
    char vendor[13];
    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';

    char brand[49] = "unknown";
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        uint32_t *words = (uint32_t *)brand;
        for (uint32_t leaf = 0; leaf < 3; leaf++) {
            cpuid(0x80000002 + leaf, &words[leaf * 4], &words[leaf * 4 + 1],
                  &words[leaf * 4 + 2], &words[leaf * 4 + 3]);
        }
        brand[48] = '\0';
    }
    const char *name = brand;
    while (*name == ' ') {
        name++;
    }
    kprintf("  vendor   %s\n  model    %s\n  cpus     %u\n  display  %lux%lu\n",
            vendor, name, arch_cpu_count(), fb_width(), fb_height());
}

static void cmd_mem(void) {
    struct heap_stats stats;
    heap_get_stats(&stats);
    uint64_t total = pmm_total_pages(), free = pmm_free_pages();
    kprintf("  total    %lu MiB\n  used     %lu MiB\n  free     %lu MiB\n",
            total * PAGE_SIZE / (1024 * 1024), (total - free) * PAGE_SIZE / (1024 * 1024),
            free * PAGE_SIZE / (1024 * 1024));
    kprintf("  heap     %lu allocations in %lu KiB\n", stats.allocations,
            (stats.slab_pages + stats.large_pages) * PAGE_SIZE / 1024);
}

static const char *memmap_type_name(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE: return "usable";
    case LIMINE_MEMMAP_RESERVED: return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE: return "ACPI reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS: return "ACPI NVS";
    case LIMINE_MEMMAP_BAD_MEMORY: return "bad memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader (reclaimed)";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel";
    case LIMINE_MEMMAP_FRAMEBUFFER: return "framebuffer";
    default: return "unknown";
    }
}

static void cmd_memmap(void) {
    size_t count;
    const struct mem_range *ranges = pmm_memory_map(&count);
    for (size_t i = 0; i < count; i++) {
        kprintf("  %p - %p  %s\n", (void *)ranges[i].base,
                (void *)(ranges[i].base + ranges[i].length), memmap_type_name(ranges[i].type));
    }
}

static void cmd_memtest(void) {
    kprintf("Running 5 million allocator operations...\n");
    memtest_run(5000000);
}

static void cmd_reboot(void) {
    kprintf("Rebooting...\n");
    interrupts_disable();
    outb(0x64, 0xfe); /* Pulse the reset line through the PS/2 controller. */
    /* If that did nothing, load an empty IDT and fault: the CPU resets. */
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } empty_idt = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(empty_idt));
    cpu_halt_forever();
}

static void run_line(const char *line) {
    while (*line == ' ') {
        line++;
    }
    if (!*line) {
        return;
    }
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(line, commands[i].name) == 0) {
            commands[i].run();
            return;
        }
    }
    kprintf("unknown command: %s (try 'help')\n", line);
}

static int wait_for_key(void) {
    for (;;) {
        int key = keyboard_read();
        if (key >= 0) {
            return key;
        }
        cpu_wait_for_interrupt();
    }
}

static void prompt(void) {
    console_set_color(CONSOLE_COLOR_ACCENT);
    kprintf("vexa> ");
    console_reset_color();
}

void monitor_run(void) {
    char line[LINE_MAX];
    size_t length = 0;

    kprintf("Type 'help' for a list of commands.\n\n");
    prompt();
    for (;;) {
        int key = wait_for_key();
        if (key == '\n') {
            kprintf("\n");
            line[length] = '\0';
            /* Trim trailing spaces so "help " still works. */
            while (length > 0 && line[length - 1] == ' ') {
                line[--length] = '\0';
            }
            run_line(line);
            length = 0;
            prompt();
        } else if (key == '\b') {
            if (length > 0) {
                length--;
                kprintf("\b \b");
            }
        } else if (key == ('l' & 0x1f)) { /* Ctrl+L */
            console_clear();
            prompt();
            line[length] = '\0';
            kprintf("%s", line);
        } else if (key == ('c' & 0x1f)) { /* Ctrl+C */
            kprintf("^C\n");
            length = 0;
            prompt();
        } else if (key >= ' ' && key < 0x7f && length < LINE_MAX - 1) {
            line[length++] = (char)key;
            kprintf("%c", key);
        }
    }
}
