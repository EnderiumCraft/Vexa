#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <vexa/arch.h>
#include <vexa/console.h>
#include <vexa/cpu.h>
#include <vexa/fb.h>
#include <vexa/fpu.h>
#include <vexa/io.h>
#include <vexa/keyboard.h>
#include <vexa/kprintf.h>
#include <vexa/memtest.h>
#include <vexa/mm.h>
#include <vexa/monitor.h>
#include <vexa/process.h>
#include <vexa/programs.h>
#include <vexa/sched.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>
#include <vexa/version.h>

#define LINE_MAX 128

struct command {
    const char *name;
    const char *usage; /* Shown in help after the name, e.g. "<program>". */
    const char *help;
    void (*run)(const char *args);
};

static void cmd_help(const char *args);
static void cmd_hello(const char *args);
static void cmd_clear(const char *args);
static void cmd_uptime(const char *args);
static void cmd_cpu(const char *args);
static void cmd_mem(const char *args);
static void cmd_memmap(const char *args);
static void cmd_memtest(const char *args);
static void cmd_programs(const char *args);
static void cmd_run(const char *args);
static void cmd_spawn(const char *args);
static void cmd_threads(const char *args);
static void cmd_ps(const char *args);
static void cmd_reboot(const char *args);

static const struct command commands[] = {
    {"help", "", "list commands", cmd_help},
    {"hello", "", "say hi", cmd_hello},
    {"clear", "", "clear the screen", cmd_clear},
    {"uptime", "", "time since boot", cmd_uptime},
    {"cpu", "", "processor and display information", cmd_cpu},
    {"mem", "", "memory usage", cmd_mem},
    {"memmap", "", "physical memory map from the firmware", cmd_memmap},
    {"memtest", "", "stress-test the memory allocators", cmd_memtest},
    {"programs", "", "list programs you can run", cmd_programs},
    {"run", "<program>", "run a program and wait for it", cmd_run},
    {"spawn", "<program>", "start a program in the background", cmd_spawn},
    {"threads", "", "list threads", cmd_threads},
    {"ps", "", "list processes", cmd_ps},
    {"reboot", "", "restart the machine", cmd_reboot},
};

#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static void pad_to(size_t used, size_t width) {
    for (; used < width; used++) {
        kprintf(" ");
    }
}

static void cmd_help(const char *args) {
    (void)args;
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        kprintf("  %s %s", commands[i].name, commands[i].usage);
        pad_to(strlen(commands[i].name) + 1 + strlen(commands[i].usage), 19);
        kprintf("%s\n", commands[i].help);
    }
}

static void cmd_hello(const char *args) {
    (void)args;
    kprintf("hi :)\nVexa " VEXA_VERSION "\n");
}

static void cmd_clear(const char *args) {
    (void)args;
    console_clear();
}

static void cmd_uptime(const char *args) {
    (void)args;
    uint64_t ms = timer_ms();
    uint64_t s = ms / 1000;
    kprintf("up %lu:%s%lu:%s%lu.%s%s%lu\n", s / 3600,
            (s / 60) % 60 < 10 ? "0" : "", (s / 60) % 60,
            s % 60 < 10 ? "0" : "", s % 60,
            ms % 1000 < 100 ? "0" : "", ms % 1000 < 10 ? "0" : "", ms % 1000);
}

static void cmd_cpu(const char *args) {
    (void)args;
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
    kprintf("  vendor   %s\n  model    %s\n  cpus     %u online (%u found)\n"
            "  vectors  %s\n  smap     %s\n  display  %lux%lu\n",
            vendor, name, cpu_online_count(), arch_cpu_count(), fpu_describe(),
            smap_enabled ? "on" : "not supported", fb_width(), fb_height());
}

static void cmd_mem(const char *args) {
    (void)args;
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
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel and programs";
    case LIMINE_MEMMAP_FRAMEBUFFER: return "framebuffer";
    default: return "unknown";
    }
}

static void cmd_memmap(const char *args) {
    (void)args;
    size_t count;
    const struct mem_range *ranges = pmm_memory_map(&count);
    for (size_t i = 0; i < count; i++) {
        kprintf("  %p - %p  %s\n", (void *)ranges[i].base,
                (void *)(ranges[i].base + ranges[i].length), memmap_type_name(ranges[i].type));
    }
}

static void cmd_memtest(const char *args) {
    (void)args;
    kprintf("Running 5 million allocator operations...\n");
    memtest_run(5000000);
}

static void cmd_programs(const char *args) {
    (void)args;
    const struct boot_program *program;
    for (size_t i = 0; (program = programs_get(i)); i++) {
        kprintf("  %s", program->name);
        pad_to(strlen(program->name), 16);
        kprintf("%lu KiB\n", (program->size + 1023) / 1024);
    }
}

static struct process *start_program(const char *name, bool detached) {
    if (!*name) {
        kprintf("which program? (try 'programs')\n");
        return NULL;
    }
    const struct boot_program *program = programs_find(name);
    if (!program) {
        kprintf("no program called %s (try 'programs')\n", name);
        return NULL;
    }
    int error;
    return process_spawn(program->name, program->data, program->size, detached, &error);
}

static void cmd_run(const char *args) {
    struct process *process = start_program(args, false);
    if (process) {
        uint32_t id = process->id;
        char name[sizeof(process->name)];
        memcpy(name, process->name, sizeof(name));
        int code = process_wait(process);
        kprintf("[proc] %s (process %u) exited with code %d\n", name, id, code);
    }
}

static void cmd_spawn(const char *args) {
    struct process *process = start_program(args, true);
    if (process) {
        kprintf("[proc] started %s in the background\n", args);
    }
}

static void print_thread(struct thread *thread, void *arg) {
    (void)arg;
    kprintf("  %u\t%s", thread->id, thread->name);
    pad_to(strlen(thread->name), 16);
    kprintf("%s", thread_state_name(thread->state));
    pad_to(strlen(thread_state_name(thread->state)), 10);
    kprintf("cpu %u  %lu ms\n", thread->cpu, thread->cpu_ms);
}

static void cmd_threads(const char *args) {
    (void)args;
    kprintf("  id\tname            state     where and time used\n");
    sched_for_each_thread(print_thread, NULL);
}

static void print_process(struct process *process, void *arg) {
    (void)arg;
    kprintf("  %u\t%s", process->id, process->name);
    pad_to(strlen(process->name), 16);
    kprintf("%s\n", process->state == PROCESS_RUNNING ? "running" : "exited");
}

static void cmd_ps(const char *args) {
    (void)args;
    kprintf("  id\tname            state\n");
    process_for_each(print_process, NULL);
}

static void cmd_reboot(const char *args) {
    (void)args;
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

static void run_line(char *line) {
    while (*line == ' ') {
        line++;
    }
    if (!*line) {
        return;
    }
    char *args = line;
    while (*args && *args != ' ') {
        args++;
    }
    if (*args) {
        *args++ = '\0';
        while (*args == ' ') {
            args++;
        }
    }
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(line, commands[i].name) == 0) {
            commands[i].run(args);
            return;
        }
    }
    kprintf("unknown command: %s (try 'help')\n", line);
}

static void prompt(void) {
    console_set_color(CONSOLE_COLOR_ACCENT);
    kprintf("vexa> ");
    console_reset_color();
}

void monitor_thread(void *unused) {
    (void)unused;
    char line[LINE_MAX];
    size_t length = 0;

    kprintf("Type 'help' for a list of commands.\n\n");
    prompt();
    for (;;) {
        int key = keyboard_read_blocking();
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
