/* The installer's parts (see main.c). */
#ifndef INSTALL_H
#define INSTALL_H

#include <stdbool.h>
#include <stdint.h>

/* Progress and errors: one line each on the output (the Installer app reads
 * them): "step: ...", "progress: N", "error: ...". */
void step(const char *format, ...) __attribute__((format(printf, 1, 2)));
void fail(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
void progress(int percent);

void random_bytes(void *out, int count);
uint32_t crc32(const void *data, uint64_t length);

/* gpt.c: a new GPT on the disk with an EFI system partition of `esp_bytes`
 * and a Vexa (ext2) partition after it, filling the disk. */
struct layout {
    uint32_t sector_size;
    uint64_t sectors;
    uint64_t esp_first, esp_sectors;   /* In sectors. */
    uint64_t root_first, root_sectors;
};
void write_gpt(int disk, struct layout *layout, uint64_t esp_bytes);

/* fat.c: a FAT32 file system on a partition, with these files in it. */
struct fat_file {
    const char *path;   /* "/EFI/BOOT/BOOTX64.EFI" */
    const void *data;
    uint32_t size;
};
void make_fat32(int partition, uint64_t bytes, uint32_t hidden_sectors, const char *label,
                const struct fat_file *files, int count);

/* ext2.c: a new, empty ext2 file system on a partition; its UUID. */
void make_ext2(int partition, uint64_t bytes, const char *label, uint8_t uuid[16]);

/* copy.c: copies the system into `to` (the new root's mount point). */
struct copy_totals {
    uint64_t bytes, done;
    int files;
};
void copy_system(const char *to, bool with_linux);

#endif
