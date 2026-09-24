#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>

void exit(int code) {
    fflush(NULL);
    vx_exit(code);
}

void abort(void) {
    fflush(NULL);
    vx_kill(vx_process_id(), VX_SIGABRT);
    vx_exit(128 + VX_SIGABRT);
}

/* ---- Memory ----
 *
 * Small blocks (up to 2 KiB) come in power-of-two sizes from 64 KiB chunks,
 * with a free list per size. Bigger blocks get their own pages from vx_map
 * and go straight back with vx_unmap. Every block has a 16-byte header. */

#define MIN_SHIFT 4
#define MAX_SHIFT 11
#define CHUNK_SIZE (64 * 1024)
#define MAGIC 0x76657861UL /* "vexa" */
#define LARGE 0xffffffffUL

struct header {
    size_t size;  /* Usable bytes (small: the class size; large: what was mapped). */
    size_t magic; /* MAGIC, plus LARGE in the upper half for mapped blocks. */
};

static void *free_lists[MAX_SHIFT + 1];
static char *chunk_next, *chunk_end;

static void *carve(size_t bytes) {
    if ((size_t)(chunk_end - chunk_next) < bytes) {
        char *chunk = vx_map(CHUNK_SIZE, VX_MAP_WRITE);
        if (!chunk) {
            return NULL;
        }
        chunk_next = chunk;
        chunk_end = chunk + CHUNK_SIZE;
    }
    void *block = chunk_next;
    chunk_next += bytes;
    return block;
}

void *malloc(size_t size) {
    if (size > ((size_t)1 << MAX_SHIFT)) {
        size_t total = (size + sizeof(struct header) + 4095) & ~(size_t)4095;
        if (total < size) {
            return NULL;
        }
        struct header *header = vx_map(total, VX_MAP_WRITE);
        if (!header) {
            return NULL;
        }
        header->size = total - sizeof(struct header);
        header->magic = MAGIC | (LARGE << 32);
        return header + 1;
    }
    int shift = MIN_SHIFT;
    while (((size_t)1 << shift) < size) {
        shift++;
    }
    struct header *header = free_lists[shift];
    if (header) {
        free_lists[shift] = *(void **)(header + 1);
    } else if (!(header = carve(sizeof(struct header) + ((size_t)1 << shift)))) {
        return NULL;
    }
    header->size = (size_t)1 << shift;
    header->magic = MAGIC;
    return header + 1;
}

void free(void *pointer) {
    if (!pointer) {
        return;
    }
    struct header *header = (struct header *)pointer - 1;
    if ((header->magic & 0xffffffff) != MAGIC) {
        static const char message[] = "free(): not a malloc pointer (or freed twice)\n";
        vx_write(2, message, sizeof(message) - 1);
        abort();
    }
    if (header->magic >> 32 == LARGE) {
        vx_unmap(header, header->size + sizeof(struct header));
        return;
    }
    int shift = MIN_SHIFT;
    while (((size_t)1 << shift) < header->size) {
        shift++;
    }
    header->magic = 0;
    *(void **)(header + 1) = free_lists[shift];
    free_lists[shift] = header;
}

void *calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        return NULL;
    }
    void *pointer = malloc(count * size);
    if (pointer) {
        memset(pointer, 0, count * size);
    }
    return pointer;
}

void *realloc(void *pointer, size_t size) {
    if (!pointer) {
        return malloc(size);
    }
    struct header *header = (struct header *)pointer - 1;
    if (size <= header->size) {
        return pointer;
    }
    void *bigger = malloc(size);
    if (bigger) {
        memcpy(bigger, pointer, header->size);
        free(pointer);
    }
    return bigger;
}

/* ---- Environment ---- */

static int environ_owned; /* environ points at our own copy once changed. */

char *getenv(const char *name) {
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') {
            return *e + n + 1;
        }
    }
    return NULL;
}

static int copy_environ(size_t extra) {
    size_t count = 0;
    while (environ && environ[count]) {
        count++;
    }
    char **copy = malloc((count + extra + 1) * sizeof(char *));
    if (!copy) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        copy[i] = environ[i];
    }
    copy[count] = NULL;
    if (environ_owned) {
        free(environ);
    }
    environ = copy;
    environ_owned = 1;
    return 0;
}

int unsetenv(const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; environ && environ[i]; i++) {
        if (strncmp(environ[i], name, n) == 0 && environ[i][n] == '=') {
            for (size_t j = i; environ[j]; j++) {
                environ[j] = environ[j + 1];
            }
            i--;
        }
    }
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (getenv(name) && !overwrite) {
        return 0;
    }
    size_t n = strlen(name), v = strlen(value);
    char *entry = malloc(n + v + 2);
    if (!entry || copy_environ(1) != 0) {
        free(entry);
        return -1;
    }
    memcpy(entry, name, n);
    entry[n] = '=';
    memcpy(entry + n + 1, value, v + 1);
    unsetenv(name);
    size_t count = 0;
    while (environ[count]) {
        count++;
    }
    environ[count] = entry;
    environ[count + 1] = NULL;
    return 0;
}

/* ---- Numbers ---- */

unsigned long strtoul(const char *text, char **end, int base) {
    const char *p = text;
    while (isspace(*p)) {
        p++;
    }
    int negative = 0;
    if (*p == '+' || *p == '-') {
        negative = *p++ == '-';
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long value = 0;
    const char *digits_start = p;
    for (;; p++) {
        int digit = isdigit(*p) ? *p - '0' : isalpha(*p) ? tolower(*p) - 'a' + 10 : 99;
        if (digit >= base) {
            break;
        }
        value = value * base + digit;
    }
    if (end) {
        *end = (char *)(p == digits_start ? text : p);
    }
    return negative ? -value : value;
}

long strtol(const char *text, char **end, int base) {
    return (long)strtoul(text, end, base);
}

int atoi(const char *text) {
    return (int)strtol(text, NULL, 10);
}

long atol(const char *text) {
    return strtol(text, NULL, 10);
}

int abs(int value) {
    return value < 0 ? -value : value;
}

long labs(long value) {
    return value < 0 ? -value : value;
}

/* Insertion sort: simple, stable, and fine for the list sizes programs here sort. */
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *)) {
    char *items = base;
    char *temp = malloc(size);
    if (!temp) {
        return;
    }
    for (size_t i = 1; i < count; i++) {
        memcpy(temp, items + i * size, size);
        size_t j = i;
        while (j > 0 && compare(items + (j - 1) * size, temp) > 0) {
            memcpy(items + j * size, items + (j - 1) * size, size);
            j--;
        }
        memcpy(items + j * size, temp, size);
    }
    free(temp);
}
