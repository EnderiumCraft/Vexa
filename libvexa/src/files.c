#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/files.h>
#include <vexa/syscall.h>

/* Whole files and folders (see <vexa/files.h>). */

void vx_join_path(char *out, size_t size, const char *dir, const char *name) {
    size_t n = strlen(dir);
    snprintf(out, size, "%s%s%s", dir, n && dir[n - 1] == '/' ? "" : "/", name);
}

/* A folder's entries, without "." and ".." (freed by the caller). */
static struct vx_dir_entry *list(const char *path, long *count) {
    *count = 0;
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        return NULL;
    }
    long capacity = 32;
    struct vx_dir_entry *entries = malloc((size_t)capacity * sizeof(*entries));
    struct vx_dir_entry batch[16];
    long n;
    while (entries && (n = vx_read_dir(handle, batch, 16)) > 0) {
        for (long i = 0; i < n && entries; i++) {
            if (!strcmp(batch[i].name, ".") || !strcmp(batch[i].name, "..")) {
                continue;
            }
            if (*count == capacity) {
                capacity *= 2;
                struct vx_dir_entry *more = realloc(entries, (size_t)capacity * sizeof(*entries));
                if (!more) {
                    free(entries);
                }
                entries = more;
                if (!entries) {
                    break;
                }
            }
            entries[(*count)++] = batch[i];
        }
    }
    vx_close(handle);
    return entries;
}

static long copy_file(const char *from, const char *to) {
    int in = vx_open(from, VX_OPEN_READ);
    if (in < 0) {
        return in;
    }
    int out = vx_open(to, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (out < 0) {
        vx_close(in);
        return out;
    }
    static char buffer[16384];
    long n, error = 0;
    while ((n = vx_read(in, buffer, sizeof(buffer))) > 0) {
        long written = vx_write(out, buffer, (size_t)n);
        if (written != n) {
            error = written < 0 ? written : -VX_ENOSPC;
            break;
        }
    }
    if (n < 0) {
        error = n;
    }
    vx_close(in);
    vx_close(out);
    return error;
}

long vx_copy_tree(const char *from, const char *to) {
    struct vx_stat stat;
    long error = vx_lstat(from, &stat);
    if (error) {
        return error;
    }
    if (vx_lstat(to, &(struct vx_stat){0}) == 0) {
        return -VX_EEXIST;
    }
    if (stat.type == VX_TYPE_SYMLINK) {
        char target[512];
        long n = vx_readlink(from, target, sizeof(target) - 1);
        if (n < 0) {
            return n;
        }
        target[n] = '\0';
        return vx_symlink(target, to);
    }
    if (stat.type != VX_TYPE_DIRECTORY) {
        return copy_file(from, to);
    }
    /* Not into itself ("copy /a to /a/b"). */
    size_t n = strlen(from);
    if (!strncmp(to, from, n) && to[n] == '/') {
        return -VX_EINVAL;
    }
    if ((error = vx_mkdir(to))) {
        return error;
    }
    long count;
    struct vx_dir_entry *entries = list(from, &count);
    if (!entries) {
        return -VX_ENOMEM;
    }
    for (long i = 0; i < count && !error; i++) {
        char a[512], b[512];
        vx_join_path(a, sizeof(a), from, entries[i].name);
        vx_join_path(b, sizeof(b), to, entries[i].name);
        error = vx_copy_tree(a, b);
    }
    free(entries);
    return error;
}

long vx_remove_tree(const char *path) {
    struct vx_stat stat;
    long error = vx_lstat(path, &stat);
    if (error) {
        return error;
    }
    if (stat.type == VX_TYPE_DIRECTORY) {
        long count;
        struct vx_dir_entry *entries = list(path, &count);
        if (!entries) {
            return -VX_ENOMEM;
        }
        for (long i = 0; i < count && !error; i++) {
            char child[512];
            vx_join_path(child, sizeof(child), path, entries[i].name);
            error = vx_remove_tree(child);
        }
        free(entries);
        if (error) {
            return error;
        }
    }
    return vx_remove(path);
}

long vx_move(const char *from, const char *to) {
    if (vx_lstat(to, &(struct vx_stat){0}) == 0) {
        return -VX_EEXIST;
    }
    long error = vx_rename(from, to);
    if (error != -VX_EXDEV) {
        return error;
    }
    if ((error = vx_copy_tree(from, to))) {
        vx_remove_tree(to); /* Don't leave half a copy. */
        return error;
    }
    return vx_remove_tree(from);
}

unsigned long long vx_tree_size(const char *path, long *files) {
    struct vx_stat stat;
    if (vx_lstat(path, &stat)) {
        return 0;
    }
    if (stat.type != VX_TYPE_DIRECTORY) {
        if (files) {
            ++*files;
        }
        return stat.type == VX_TYPE_FILE ? stat.size : 0;
    }
    long count;
    struct vx_dir_entry *entries = list(path, &count);
    unsigned long long total = 0;
    for (long i = 0; entries && i < count; i++) {
        char child[512];
        vx_join_path(child, sizeof(child), path, entries[i].name);
        total += vx_tree_size(child, files);
    }
    free(entries);
    return total;
}

void vx_unique_name(const char *dir, const char *name, char *out, size_t size) {
    char path[512];
    vx_join_path(path, sizeof(path), dir, name);
    if (vx_lstat(path, &(struct vx_stat){0})) {
        snprintf(out, size, "%s", name);
        return;
    }
    /* The number goes before the extension. */
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) {
        dot = name + strlen(name);
    }
    for (int i = 2; i < 1000; i++) {
        snprintf(out, size, "%.*s %d%s", (int)(dot - name), name, i, dot);
        vx_join_path(path, sizeof(path), dir, out);
        if (vx_lstat(path, &(struct vx_stat){0})) {
            return;
        }
    }
}
