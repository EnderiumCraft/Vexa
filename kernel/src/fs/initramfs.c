#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/* The initramfs is a ustar archive the bootloader loads next to the kernel.
 * Its files are copied into the root tmpfs at boot. */

struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char link_name[100];
    char magic[6]; /* "ustar\0" */
    char version[2];
    char user_name[32];
    char group_name[32];
    char dev_major[8];
    char dev_minor[8];
    char prefix[155];
    char padding[12];
};

_Static_assert(sizeof(struct ustar_header) == 512, "tar headers are 512 bytes");

static uint64_t parse_octal(const char *field, size_t length) {
    uint64_t value = 0;
    for (size_t i = 0; i < length && field[i] >= '0' && field[i] <= '7'; i++) {
        value = value * 8 + (field[i] - '0');
    }
    return value;
}

static size_t field_length(const char *field, size_t max) {
    size_t n = 0;
    while (n < max && field[n]) {
        n++;
    }
    return n;
}

int initramfs_unpack(const void *data, size_t size) {
    const uint8_t *pos = data, *end = pos + size;
    int files = 0, directories = 0, links = 0;
    char path[VX_PATH_MAX];

    while (pos + sizeof(struct ustar_header) <= end) {
        const struct ustar_header *header = (const void *)pos;
        if (header->name[0] == '\0') {
            break; /* Two zero blocks end the archive. */
        }
        if (memcmp(header->magic, "ustar", 5) != 0) {
            kprintf("[initramfs] not a ustar archive\n");
            return -VX_EINVAL;
        }
        uint64_t file_size = parse_octal(header->size, sizeof(header->size));
        const uint8_t *contents = pos + sizeof(*header);
        if (file_size > (uint64_t)(end - contents)) {
            kprintf("[initramfs] archive is truncated\n");
            return -VX_EINVAL;
        }

        /* The full name is prefix + "/" + name; drop a leading "./". */
        size_t length = 0;
        size_t prefix_length = field_length(header->prefix, sizeof(header->prefix));
        if (prefix_length) {
            memcpy(path, header->prefix, prefix_length);
            path[prefix_length] = '/';
            length = prefix_length + 1;
        }
        size_t name_length = field_length(header->name, sizeof(header->name));
        memcpy(path + length, header->name, name_length);
        length += name_length;
        const char *name = path;
        if (length >= 2 && name[0] == '.' && name[1] == '/') {
            name += 2;
            length -= 2;
        }
        while (length > 0 && name[length - 1] == '/') {
            length--;
        }

        if (length > 0) {
            if (header->type == '5') {
                int error = vfs_mkdir(name, length);
                if (error && error != -VX_EEXIST) {
                    kprintf("[initramfs] mkdir failed: %s\n", vfs_error_name(error));
                }
                directories++;
            } else if (header->type == '2') {
                char target[sizeof(header->link_name) + 1];
                size_t target_length = field_length(header->link_name, sizeof(header->link_name));
                memcpy(target, header->link_name, target_length);
                target[target_length] = '\0';
                int error = vfs_symlink(target, name, length);
                if (error) {
                    kprintf("[initramfs] could not make a symbolic link: %s\n",
                            vfs_error_name(error));
                }
                links++;
            } else if (header->type == '0' || header->type == '\0') {
                struct file *file;
                int error = vfs_open(name, length,
                                     VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE, &file);
                if (!error) {
                    int64_t written = vfs_write(file, contents, file_size);
                    if (written != (int64_t)file_size) {
                        kprintf("[initramfs] could not write a file (%s)\n",
                                written < 0 ? vfs_error_name((int)written) : "short write");
                    }
                    vfs_close(file);
                    files++;
                } else {
                    kprintf("[initramfs] could not create a file: %s\n", vfs_error_name(error));
                }
            }
        }
        pos = contents + ((file_size + 511) & ~511ULL);
    }
    kprintf("[initramfs] unpacked %d files, %d directories and %d symbolic links\n", files,
            directories, links);
    return 0;
}
