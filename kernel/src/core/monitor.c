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
#include <vexa/pci.h>
#include <vexa/monitor.h>
#include <vexa/process.h>
#include <vexa/block.h>
#include <vexa/sched.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>
#include <vexa/version.h>
#include <vexa/vfs.h>

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
static void cmd_ls(const char *args);
static void cmd_cat(const char *args);
static void cmd_write(const char *args);
static void cmd_mkdir(const char *args);
static void cmd_rm(const char *args);
static void cmd_mount(const char *args);
static void cmd_disks(const char *args);
static void cmd_pci(const char *args);
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
    {"ls", "[path]", "list a directory", cmd_ls},
    {"cat", "<file>", "show a file", cmd_cat},
    {"write", "<file> <text>", "write text to a file", cmd_write},
    {"mkdir", "<path>", "make a directory", cmd_mkdir},
    {"rm", "<path>", "remove a file or empty directory", cmd_rm},
    {"mount", "", "list mounted file systems", cmd_mount},
    {"disks", "", "list disks and partitions", cmd_disks},
    {"pci", "", "list PCI devices", cmd_pci},
    {"programs", "", "list programs in /bin", cmd_programs},
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
        pad_to(strlen(commands[i].name) + 1 + strlen(commands[i].usage), 22);
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

static const char *type_name(uint32_t type) {
    switch (type) {
    case VX_TYPE_DIRECTORY: return "dir";
    case VX_TYPE_CHAR_DEVICE: return "char";
    case VX_TYPE_BLOCK_DEVICE: return "block";
    case VX_TYPE_SYMLINK: return "link";
    default: return "";
    }
}

static void print_size(uint64_t bytes) {
    if (bytes < 10 * 1024) {
        kprintf("%lu B", bytes);
    } else if (bytes < 10 * 1024 * 1024) {
        kprintf("%lu KiB", bytes / 1024);
    } else {
        kprintf("%lu MiB", bytes / (1024 * 1024));
    }
}

/* Lists a directory. Entries come in the order the file system keeps them. */
static void list_directory(const char *path) {
    struct file *dir;
    int error = vfs_open(path, strlen(path), VX_OPEN_READ, &dir);
    if (error) {
        kprintf("ls: %s: %s\n", path, vfs_error_name(error));
        return;
    }
    struct vx_dir_entry *entry = kmalloc(sizeof(*entry));
    char *child = kmalloc(VX_PATH_MAX);
    size_t path_length = strlen(path);
    while (entry && child && vfs_read_dir(dir, entry) == 1) {
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
            continue;
        }
        kprintf("  %s", entry->name);
        pad_to(entry->name_length, 20);
        struct vx_stat stat;
        memcpy(child, path, path_length);
        size_t n = path_length;
        if (n == 0 || child[n - 1] != '/') {
            child[n++] = '/';
        }
        memcpy(child + n, entry->name, entry->name_length + 1);
        if (entry->type == VX_TYPE_FILE && vfs_stat(child, n + entry->name_length, &stat) == 0) {
            print_size(stat.size);
        } else {
            kprintf("%s", type_name(entry->type));
        }
        kprintf("\n");
    }
    kfree(entry);
    kfree(child);
    vfs_close(dir);
}

static void cmd_ls(const char *args) {
    list_directory(*args ? args : "/");
}

static void cmd_cat(const char *args) {
    struct file *file;
    int error = vfs_open(args, strlen(args), VX_OPEN_READ, &file);
    if (error) {
        kprintf("cat: %s: %s\n", args, vfs_error_name(error));
        return;
    }
    char buffer[512];
    int64_t n;
    bool ends_with_newline = true;
    while ((n = vfs_read(file, buffer, sizeof(buffer))) > 0) {
        kwrite(buffer, n);
        ends_with_newline = buffer[n - 1] == '\n';
    }
    if (n < 0) {
        kprintf("cat: %s: %s\n", args, vfs_error_name((int)n));
    } else if (!ends_with_newline) {
        kprintf("\n");
    }
    vfs_close(file);
}

static void cmd_write(const char *args) {
    const char *text = args;
    while (*text && *text != ' ') {
        text++;
    }
    size_t path_length = text - args;
    while (*text == ' ') {
        text++;
    }
    if (!path_length) {
        kprintf("usage: write <file> <text>\n");
        return;
    }
    struct file *file;
    int error = vfs_open(args, path_length, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE,
                         &file);
    if (error) {
        kprintf("write: %s\n", vfs_error_name(error));
        return;
    }
    int64_t n = vfs_write(file, text, strlen(text));
    if (n >= 0) {
        n = vfs_write(file, "\n", 1);
    }
    if (n < 0) {
        kprintf("write: %s\n", vfs_error_name((int)n));
    }
    vfs_close(file);
}

static void cmd_mkdir(const char *args) {
    int error = vfs_mkdir(args, strlen(args));
    if (error) {
        kprintf("mkdir: %s: %s\n", args, vfs_error_name(error));
    }
}

static void cmd_rm(const char *args) {
    int error = vfs_remove(args, strlen(args));
    if (error) {
        kprintf("rm: %s: %s\n", args, vfs_error_name(error));
    }
}

static void cmd_mount(const char *args) {
    (void)args;
    vfs_lock();
    for (struct mount *mount = vfs_mounts(); mount; mount = mount->next) {
        kprintf("  %s", mount->path);
        pad_to(strlen(mount->path), 16);
        kprintf("%s", mount->fs_name);
        pad_to(strlen(mount->fs_name), 8);
        kprintf("%s%s\n", mount->source, mount->read_only ? " (read-only)" : "");
    }
    vfs_unlock();
}

static void cmd_disks(const char *args) {
    (void)args;
    struct block_device *device = block_first();
    if (!device) {
        kprintf("no disks found\n");
    }
    for (; device; device = device->next) {
        kprintf("  %s", device->name);
        pad_to(strlen(device->name), 12);
        print_size(block_size_bytes(device));
        kprintf("%s\n", device->parent ? "  (partition)" : "");
    }
}

static void cmd_pci(const char *args) {
    (void)args;
    for (struct pci_device *d = pci_first(); d; d = d->next) {
        kprintf("  %x:%x.%u  %x:%x  %s\n", d->bus, d->slot, d->function, d->vendor_id,
                d->device_id, pci_class_name(d));
    }
}

static void cmd_programs(const char *args) {
    (void)args;
    list_directory("/bin");
}

static bool strchr_simple(const char *s, char c) {
    for (; *s; s++) {
        if (*s == c) {
            return true;
        }
    }
    return false;
}

static struct process *start_program(const char *name, bool detached) {
    if (!*name) {
        kprintf("which program? (try 'programs')\n");
        return NULL;
    }
    /* A bare name means /bin/<name>. */
    char path[128] = "/bin/";
    if (strchr_simple(name, '/')) {
        return process_spawn(name, detached);
    }
    size_t n = strlen(name);
    if (n > sizeof(path) - 6) {
        kprintf("program name too long\n");
        return NULL;
    }
    memcpy(path + 5, name, n + 1);
    return process_spawn(path, detached);
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
