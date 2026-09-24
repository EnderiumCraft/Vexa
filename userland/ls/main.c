/* ls: list directories. -l adds sizes and types, -a shows names starting with a dot. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>

static bool long_format, show_all;

static int compare(const void *a, const void *b) {
    return strcmp(((const struct vx_dir_entry *)a)->name, ((const struct vx_dir_entry *)b)->name);
}

static const char *type_name(unsigned type) {
    switch (type) {
    case VX_TYPE_DIRECTORY: return "dir";
    case VX_TYPE_CHAR_DEVICE: return "char";
    case VX_TYPE_BLOCK_DEVICE: return "block";
    case VX_TYPE_SYMLINK: return "link";
    default: return "file";
    }
}

static void print_size(unsigned long long bytes) {
    if (bytes < 10 * 1024) {
        printf("%8llu B  ", bytes);
    } else if (bytes < 10ULL * 1024 * 1024) {
        printf("%6llu KiB  ", bytes / 1024);
    } else {
        printf("%6llu MiB  ", bytes / (1024 * 1024));
    }
}

static int list(const char *path, bool with_title) {
    struct vx_stat st;
    long error = vx_stat(path, &st);
    if (error) {
        fprintf(stderr, "ls: %s: %s\n", path, vx_strerror(error));
        return 1;
    }
    if (st.type != VX_TYPE_DIRECTORY) {
        if (long_format) {
            print_size(st.size);
        }
        printf("%s\n", path);
        return 0;
    }
    int dir = vx_open(path, VX_OPEN_READ);
    if (dir < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, vx_strerror(dir));
        return 1;
    }
    size_t count = 0, capacity = 64;
    struct vx_dir_entry *entries = malloc(capacity * sizeof(*entries));
    long n;
    while (entries && (n = vx_read_dir(dir, entries + count, capacity - count)) > 0) {
        count += n;
        if (count == capacity) {
            capacity *= 2;
            entries = realloc(entries, capacity * sizeof(*entries));
        }
    }
    vx_close(dir);
    if (!entries) {
        return 1;
    }
    qsort(entries, count, sizeof(*entries), compare);
    if (with_title) {
        printf("%s:\n", path);
    }
    int column = 0;
    for (size_t i = 0; i < count; i++) {
        const char *name = entries[i].name;
        if (name[0] == '.' && !show_all) {
            continue;
        }
        if (long_format) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, name);
            if (vx_stat(full, &st) == 0 && st.type == VX_TYPE_FILE) {
                print_size(st.size);
            } else {
                printf("%10s  ", type_name(entries[i].type));
            }
            printf("%s%s\n", name, entries[i].type == VX_TYPE_DIRECTORY ? "/" : "");
        } else {
            /* Directories are marked with a slash; names in columns of 20. */
            char shown[300];
            snprintf(shown, sizeof(shown), "%s%s", name, entries[i].type == VX_TYPE_DIRECTORY ? "/" : "");
            printf("%-20s", shown);
            if (strlen(shown) >= 20 || ++column == 4) {
                printf("\n");
                column = 0;
            }
        }
    }
    if (!long_format && column) {
        printf("\n");
    }
    free(entries);
    return 0;
}

int main(int argc, char **argv) {
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (const char *f = argv[first] + 1; *f; f++) {
            if (*f == 'l') long_format = true;
            else if (*f == 'a') show_all = true;
            else {
                fprintf(stderr, "usage: ls [-la] [path...]\n");
                return 2;
            }
        }
    }
    if (first == argc) {
        return list(".", false);
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        status |= list(argv[i], argc - first > 1);
    }
    return status;
}
