/* install: puts Vexa on a disk, which then boots by itself (BIOS or UEFI).
 *
 *   install --list            the disks it could go on
 *   install [--yes] [--no-linux] vda
 *
 * Everything on the disk is erased. It gets a GPT with two partitions: an
 * EFI system partition (FAT32: Limine, its configuration, the kernel) and
 * the system itself (ext2: everything in / now, and the Linux programs from
 * the boot CD unless --no-linux). Limine boots it with root=UUID=<the ext2's
 * UUID>, so the kernel finds it on whatever controller it's on.
 *
 * Its output is one line per step ("step: ...", "progress: N", "error: ..."),
 * which the Installer app shows. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vexa/syscall.h>
#include "install.h"

#define ESP_BYTES (128ULL * 1024 * 1024)

void step(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printf("step: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

void fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printf("error: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
    exit(1);
}

void progress(int percent) {
    printf("progress: %d\n", percent);
    fflush(stdout);
}

void random_bytes(void *out, int count) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, out, (size_t)count) != count) {
        fail("no random numbers (/dev/urandom)");
    }
    close(fd);
}

uint32_t crc32(const void *data, uint64_t length) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xedb88320U ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    uint32_t crc = 0xffffffffU;
    const uint8_t *p = data;
    for (uint64_t i = 0; i < length; i++) {
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffU;
}

/* ---- Disks ---- */

static void list_disks(void) {
    static struct vx_device_info devices[256];
    long n = vx_device_list(devices, 256, NULL);
    for (long i = 0; i < n && i < 256; i++) {
        const struct vx_device_info *d = &devices[i];
        if (d->kind != VX_DEVICE_DISK || strncmp(d->location, "/dev/", 5) ||
            strstr(d->name, "CD")) {
            continue;
        }
        printf("%-8s %-24s %s\n", d->location + 5, d->name, d->details);
    }
}

static bool in_use(const char *disk) {
    struct vx_mount_info mounts[32];
    long n = vx_mounts(mounts, 32);
    for (long i = 0; i < n && i < 32; i++) {
        if (!strncmp(mounts[i].source, disk, strlen(disk))) {
            return true;
        }
    }
    return false;
}

static void partition_name(char *out, size_t size, const char *disk, int number) {
    size_t n = strlen(disk);
    bool digit = n && disk[n - 1] >= '0' && disk[n - 1] <= '9';
    snprintf(out, size, "%s%s%d", disk, digit ? "p" : "", number);
}

static void rescan(const char *disk) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", disk);
    int fd = open(path, O_RDWR);
    if (fd < 0 || vx_control(fd, VX_BLOCK_RESCAN, NULL, 0) < 0) {
        fail("the new partitions couldn't be read (is the disk in use?)");
    }
    close(fd);
}

/* ---- The boot files ---- */

/* Where the boot CD is (its kernel and Limine's files are copied from it). */
static const char *boot_media(void) {
    static char path[64];
    struct stat st;
    if (stat("/cdrom/boot/vexa-kernel", &st) == 0) {
        return "/cdrom";
    }
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/mnt/cd%d/boot/vexa-kernel", i);
        if (stat(path, &st) == 0) {
            snprintf(path, sizeof(path), "/mnt/cd%d", i);
            return path;
        }
    }
    return NULL;
}

static void *read_whole(const char *path, uint32_t *size) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        fail("can't read %s", path);
    }
    void *data = malloc((size_t)st.st_size + 1);
    if (!data || read(fd, data, (size_t)st.st_size) != st.st_size) {
        fail("can't read %s", path);
    }
    close(fd);
    *size = (uint32_t)st.st_size;
    return data;
}

static void write_whole(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text)) {
        fail("can't write %s", path);
    }
    close(fd);
}

static char limine_conf[2048];

static void make_limine_conf(const uint8_t uuid[16]) {
    char id[40];
    snprintf(id, sizeof(id),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", uuid[0],
             uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    snprintf(limine_conf, sizeof(limine_conf),
             "timeout: 3\n\n"
             "# Vexa, installed: the system is the ext2 file system with this UUID.\n\n"
             "/Vexa\n"
             "    protocol: limine\n"
             "    path: boot():/boot/vexa-kernel\n"
             "    cmdline: root=UUID=%s\n\n"
             "/Vexa (safe mode: no ACPI, legacy PIC and PIT)\n"
             "    protocol: limine\n"
             "    path: boot():/boot/vexa-kernel\n"
             "    cmdline: root=UUID=%s acpi=off noapic\n\n"
             "/Vexa (kernel monitor, for when something is broken)\n"
             "    protocol: limine\n"
             "    path: boot():/boot/vexa-kernel\n"
             "    cmdline: root=UUID=%s monitor\n\n"
             "/Vexa (console: a shell, not the desktop)\n"
             "    protocol: limine\n"
             "    path: boot():/boot/vexa-kernel\n"
             "    cmdline: root=UUID=%s console\n\n"
             "/Vexa at 1024x768\n"
             "    protocol: limine\n"
             "    path: boot():/boot/vexa-kernel\n"
             "    cmdline: root=UUID=%s\n"
             "    resolution: 1024x768x32\n\n"
             "/Vexa at 1280x720\n"
             "    protocol: limine\n"
             "    path: boot():/boot/vexa-kernel\n"
             "    cmdline: root=UUID=%s\n"
             "    resolution: 1280x720x32\n",
             id, id, id, id, id, id);
    step("the system's UUID is %s", id);
}

int main(int argc, char **argv) {
    bool yes = false, with_linux = true;
    const char *disk = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) {
            list_disks();
            return 0;
        } else if (!strcmp(argv[i], "--yes")) {
            yes = true;
        } else if (!strcmp(argv[i], "--no-linux")) {
            with_linux = false;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: install --list | install [--yes] [--no-linux] DISK\n");
            return 2;
        } else {
            disk = !strncmp(argv[i], "/dev/", 5) ? argv[i] + 5 : argv[i];
        }
    }
    if (!disk) {
        fprintf(stderr, "usage: install --list | install [--yes] [--no-linux] DISK\n"
                        "The disks Vexa could go on:\n");
        list_disks();
        return 2;
    }

    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", disk);
    int fd = open(path, O_RDWR);
    struct vx_block_info info;
    if (fd < 0 || vx_control(fd, VX_BLOCK_INFO, &info, sizeof(info)) < 0) {
        fail("there's no disk %s", disk);
    }
    if (info.partition) {
        fail("%s is a partition: Vexa goes on a whole disk", disk);
    }
    if (info.size < 512ULL * 1024 * 1024) {
        fail("%s is too small (Vexa needs 512 MiB)", disk);
    }
    /* What's mounted from it in /mnt (an old installation, a stick's FAT)
     * goes; the system's own file systems can't. */
    if (in_use(disk) && vx_control(fd, VX_BLOCK_EJECT, NULL, 0) < 0) {
        fail("%s is in use (Vexa itself, or /home, is on it)", disk);
    }
    const char *media = boot_media();
    if (!media) {
        fail("the boot CD isn't there (Vexa is installed from it)");
    }
    if (!yes) {
        printf("Everything on %s (%llu MiB) will be erased. Type yes to go on: ", disk,
               (unsigned long long)(info.size >> 20));
        fflush(stdout);
        char answer[16] = "";
        if (!fgets(answer, sizeof(answer), stdin) || strncmp(answer, "yes", 3)) {
            printf("Nothing was changed.\n");
            return 1;
        }
    }

    /* 1. The partitions. */
    step("partitioning %s (%llu MiB)", disk, (unsigned long long)(info.size >> 20));
    struct layout layout = {.sector_size = info.sector_size, .sectors = info.size / info.sector_size};
    uint64_t esp = info.sector_size == 512 ? ESP_BYTES : ESP_BYTES * 3; /* (FAT32's minimum.) */
    write_gpt(fd, &layout, esp);
    fsync(fd);
    close(fd);
    rescan(disk);
    char esp_name[32], root_name[32];
    partition_name(esp_name, sizeof(esp_name), disk, 1);
    partition_name(root_name, sizeof(root_name), disk, 2);

    /* 2. The system's file system. */
    step("making the file system on %s", root_name);
    snprintf(path, sizeof(path), "/dev/%s", root_name);
    int root = open(path, O_RDWR);
    if (root < 0) {
        fail("there's no %s", root_name);
    }
    uint8_t uuid[16];
    make_ext2(root, layout.root_sectors * layout.sector_size, "Vexa", uuid);
    close(root);

    /* 3. The boot partition: Limine and the kernel. */
    step("making the boot partition on %s", esp_name);
    make_limine_conf(uuid);
    char source[128];
    struct fat_file files[6];
    int count = 0;
    static const char *const from[][2] = {
        {"/boot/vexa-kernel", "/boot/vexa-kernel"},
        {"/boot/limine/limine-bios.sys", "/boot/limine/limine-bios.sys"},
        {"/EFI/BOOT/BOOTX64.EFI", "/EFI/BOOT/BOOTX64.EFI"},
        {"/EFI/BOOT/BOOTIA32.EFI", "/EFI/BOOT/BOOTIA32.EFI"},
    };
    for (int i = 0; i < 4; i++) {
        snprintf(source, sizeof(source), "%s%s", media, from[i][0]);
        uint32_t size;
        void *data = read_whole(source, &size);
        files[count++] = (struct fat_file){from[i][1], data, size};
    }
    files[count++] = (struct fat_file){"/boot/limine/limine.conf", limine_conf,
                                       (uint32_t)strlen(limine_conf)};
    snprintf(path, sizeof(path), "/dev/%s", esp_name);
    int boot = open(path, O_RDWR);
    if (boot < 0) {
        fail("there's no %s", esp_name);
    }
    make_fat32(boot, layout.esp_sectors * layout.sector_size, (uint32_t)layout.esp_first,
               "VEXA BOOT", files, count);
    close(boot);

    /* 4. The system itself. */
    rescan(disk); /* (Mounts the new file system at /mnt/<root>.) */
    char target[64];
    snprintf(target, sizeof(target), "/mnt/%s", root_name);
    struct stat st;
    if (stat(target, &st) != 0 || !in_use(root_name)) {
        fail("the new file system couldn't be mounted");
    }
    step("copying the system to %s%s", root_name, with_linux ? " (with the Linux programs)" : "");
    copy_system(target, with_linux);
    struct vx_system_info about;
    vx_system_info(&about);
    char note[160];
    snprintf(note, sizeof(note), "Vexa %s, installed on %s at %lld.\n", about.version, disk,
             (long long)vx_time());
    snprintf(path, sizeof(path), "%s/etc/installed", target);
    write_whole(path, note);

    /* 5. Limine's BIOS boot code (UEFI finds EFI/BOOT/BOOTX64.EFI by itself). */
    step("putting the BIOS boot code on %s", disk);
    snprintf(path, sizeof(path), "/bin/limine bios-install /dev/%s > /dev/null", disk);
    if (system(path) != 0) {
        fail("Limine's BIOS boot code couldn't be put on %s", disk);
    }
    sync();
    step("done: Vexa is on %s; take the CD out and restart to use it", disk);
    return 0;
}
