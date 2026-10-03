/* Copies the running system onto the new disk: everything in the root but
 * what's made at each boot (/dev, /proc, /tmp, /run) and the other disks
 * (/mnt); the Linux programs (/linux) too, unless left out: their parts on
 * the boot CD (/linux/usr -> /cdrom/linux/usr...) are copied as the
 * directories they are there (and Doom's game files, as files). */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "install.h"

static uint64_t total_bytes, done_bytes;
static int files_done, last_percent = -1;
static bool counting;
static char buffer[65536];

static bool left_out(const char *name, bool with_linux) {
    static const char *const skipped[] = {"dev", "proc", "tmp", "run", "mnt", "cdrom", "lost+found"};
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
        if (!strcmp(name, skipped[i])) {
            return true;
        }
    }
    return !with_linux && !strcmp(name, "linux");
}

static void tick(uint64_t bytes) {
    done_bytes += bytes;
    int percent = total_bytes ? (int)(done_bytes * 100 / total_bytes) : 100;
    if (percent != last_percent) {
        last_percent = percent;
        progress(percent);
    }
}

static void copy_file(const char *from, const char *to, mode_t mode, off_t size) {
    if (counting) {
        total_bytes += (uint64_t)size;
        return;
    }
    int in = open(from, O_RDONLY);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777);
    if (in < 0 || out < 0) {
        fail("can't copy %s", from);
    }
    for (;;) {
        ssize_t n = read(in, buffer, sizeof(buffer));
        if (n < 0) {
            fail("can't read %s", from);
        }
        if (n == 0) {
            break;
        }
        if (write(out, buffer, (size_t)n) != n) {
            fail("can't write %s (is the disk full?)", to);
        }
        tick((uint64_t)n);
    }
    close(in);
    close(out);
    chmod(to, mode & 07777);
    files_done++;
}

static void copy_tree(const char *from, const char *to);

static void copy_entry(const char *from, const char *to) {
    struct stat st;
    if (lstat(from, &st) != 0) {
        return;
    }
    if (S_ISLNK(st.st_mode)) {
        char target[1024];
        ssize_t n = readlink(from, target, sizeof(target) - 1);
        if (n < 0) {
            return;
        }
        target[n] = '\0';
        struct stat real;
        if (!strncmp(target, "/cdrom/", 7) && stat(target, &real) == 0) {
            /* What's on the boot CD (the Linux files, Doom's): copied for real. */
            if (S_ISDIR(real.st_mode)) {
                copy_tree(target, to);
            } else {
                copy_file(target, to, real.st_mode, real.st_size);
            }
        } else if (!counting) {
            symlink(target, to);
        }
    } else if (S_ISDIR(st.st_mode)) {
        copy_tree(from, to);
    } else if (S_ISREG(st.st_mode)) {
        copy_file(from, to, st.st_mode, st.st_size);
    }
}

static void copy_tree(const char *from, const char *to) {
    if (!counting) {
        struct stat st;
        mkdir(to, 0755);
        if (stat(from, &st) == 0) {
            chmod(to, st.st_mode & 07777);
        }
    }
    DIR *dir = opendir(from);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        char *a = malloc(strlen(from) + strlen(entry->d_name) + 2);
        char *b = malloc(strlen(to) + strlen(entry->d_name) + 2);
        if (!a || !b) {
            fail("out of memory");
        }
        sprintf(a, "%s/%s", strcmp(from, "/") ? from : "", entry->d_name);
        sprintf(b, "%s/%s", to, entry->d_name);
        copy_entry(a, b);
        free(a);
        free(b);
    }
    closedir(dir);
}

static void copy_root(const char *to, bool with_linux) {
    DIR *dir = opendir("/");
    if (!dir) {
        fail("can't read /");
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            left_out(entry->d_name, with_linux)) {
            continue;
        }
        char a[300], b[600];
        snprintf(a, sizeof(a), "/%s", entry->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, entry->d_name);
        copy_entry(a, b);
    }
    closedir(dir);
}

void copy_system(const char *to, bool with_linux) {
    counting = true;
    copy_root(to, with_linux);
    step("copying %llu MiB", (unsigned long long)(total_bytes >> 20));
    counting = false;
    copy_root(to, with_linux);
    /* What's made at each boot needs somewhere to go. */
    static const char *const fresh[] = {"dev", "proc", "tmp", "run", "mnt", "home"};
    for (size_t i = 0; i < sizeof(fresh) / sizeof(fresh[0]); i++) {
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", to, fresh[i]);
        mkdir(path, 0755);
    }
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s/tmp", to);
    chmod(tmp, 01777);
    step("copied %d files", files_done);
}
