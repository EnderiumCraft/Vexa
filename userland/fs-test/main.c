/*
 * Exercises the file system calls from user mode: creating, writing, reading,
 * seeking, listing and removing files and directories, and the errors that
 * must come back when things are used wrongly.
 *
 * With no disk it works in /tmp. Run it as `run fs-test` and it prints
 * "fs-test: passed" if everything behaved. If a writable disk is mounted at
 * /mnt/vda1, it repeats the checks there too.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

static int failures;

#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            printf("fs-test: FAILED line %d: %s\n", __LINE__, #condition);           \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static void join(char *out, const char *dir, const char *name) {
    size_t n = strlen(dir);
    memcpy(out, dir, n);
    out[n] = '/';
    memcpy(out + n + 1, name, strlen(name) + 1);
}

static void test_in(const char *base) {
    char dir[128], file[128], missing[128], inner[128], below_file[160];
    join(dir, base, "fs-test-dir");
    join(file, dir, "numbers.txt");
    join(missing, dir, "missing.txt");
    join(inner, dir, "inner");

    vx_remove(file); /* Left over from an earlier run? */
    vx_remove(inner);
    vx_remove(dir);

    CHECK(vx_mkdir(dir) == 0);
    CHECK(vx_mkdir(dir) == -VX_EEXIST);
    CHECK(vx_mkdir(inner) == 0);

    /* Write a file bigger than a page, in pieces. */
    int h = vx_open(file, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    CHECK(h >= 0);
    char line[32];
    long total = 0;
    for (int i = 0; i < 1000; i++) {
        int n = 0;
        int v = i;
        char digits[8];
        do {
            digits[n++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        int len = 0;
        while (n) {
            line[len++] = digits[--n];
        }
        line[len++] = '\n';
        CHECK(vx_write(h, line, len) == len);
        total += len;
    }
    CHECK(vx_read(h, line, 4) == -VX_EACCES); /* Opened for writing only. */
    CHECK(vx_close(h) == 0);
    CHECK(vx_close(h) == -VX_EBADF);

    struct vx_stat st;
    CHECK(vx_stat(file, &st) == 0);
    CHECK((long)st.size == total && st.type == VX_TYPE_FILE);

    /* Read it back, including across page boundaries after a seek. */
    h = vx_open(file, VX_OPEN_READ);
    CHECK(h >= 0);
    char buffer[16];
    CHECK(vx_read(h, buffer, 4) == 4 && memcmp(buffer, "0\n1\n", 4) == 0);
    CHECK(vx_seek(h, -4, VX_SEEK_END) == total - 4);
    CHECK(vx_read(h, buffer, 16) == 4 && memcmp(buffer, "999\n", 4) == 0);
    CHECK(vx_read(h, buffer, 16) == 0);
    CHECK(vx_write(h, "x", 1) == -VX_EACCES); /* Opened for reading only. */
    CHECK(vx_read(h, (void *)0xffff800000000000, 4) == -VX_EFAULT);
    CHECK(vx_handle_stat(h, &st) == 0 && (long)st.size == total);
    vx_close(h);

    /* Appending. */
    h = vx_open(file, VX_OPEN_APPEND);
    CHECK(h >= 0 && vx_write(h, "end\n", 4) == 4);
    vx_close(h);
    CHECK(vx_stat(file, &st) == 0 && (long)st.size == total + 4);

    /* Listing finds both entries. */
    h = vx_open(dir, VX_OPEN_READ);
    CHECK(h >= 0);
    static struct vx_dir_entry entries[8];
    long count = vx_read_dir(h, entries, 8);
    bool saw_file = false, saw_inner = false;
    for (long i = 0; i < count; i++) {
        saw_file |= strcmp(entries[i].name, "numbers.txt") == 0 && entries[i].type == VX_TYPE_FILE;
        saw_inner |= strcmp(entries[i].name, "inner") == 0 && entries[i].type == VX_TYPE_DIRECTORY;
    }
    CHECK(saw_file && saw_inner);
    CHECK(vx_write(h, "x", 1) == -VX_EACCES);
    vx_close(h);

    /* Errors. */
    CHECK(vx_open(missing, VX_OPEN_READ) == -VX_ENOENT);
    CHECK(vx_open(dir, VX_OPEN_WRITE) == -VX_EISDIR);
    CHECK(vx_remove(dir) == -VX_ENOTEMPTY);
    join(below_file, file, "x");
    CHECK(vx_open(below_file, VX_OPEN_READ) == -VX_ENOTDIR);

    /* Clean up. */
    CHECK(vx_remove(file) == 0);
    CHECK(vx_remove(inner) == 0);
    CHECK(vx_remove(dir) == 0);
    CHECK(vx_stat(dir, &st) == -VX_ENOENT);
}

int main(void) {
    test_in("/tmp");
    struct vx_stat st;
    bool disk = vx_stat("/mnt/vda1", &st) == 0;
    if (disk) {
        test_in("/mnt/vda1");
    }
    if (failures) {
        printf("fs-test: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("fs-test: passed (in /tmp%s)\n", disk ? " and on the disk at /mnt/vda1" : "");
    return 0;
}
