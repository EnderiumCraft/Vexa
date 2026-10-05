/*
 * archive: zip and tar.gz (and plain tar and .gz) files.
 *
 *     archive list FILE             what's in it
 *     archive extract FILE [DIR]    unpacks it into DIR (by default, a folder
 *                                   next to it named after it)
 *     archive create OUT FILE...    packs files and folders into OUT: a .zip,
 *                                   or a .tar.gz (.tgz) or .tar
 *
 * Files' Extract and Compress run it. zlib does the compressing.
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define CHUNK 65536

static int file_count;

static bool ends_with(const char *s, const char *end) {
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !strcasecmp(s + a - b, end);
}

static int fail(const char *what, const char *name) {
    fprintf(stderr, "archive: %s: %s\n", what, name);
    return 1;
}

/* A name from an archive is only used if it stays inside the folder. */
static bool safe_name(const char *name) {
    if (!name[0] || name[0] == '/') {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (p[0] == '.' && p[1] == '.' && (p == name || p[-1] == '/') && (p[2] == '/' || !p[2])) {
            return false;
        }
    }
    return true;
}

/* Makes the folders in `path` up to its last slash. */
static void make_parents(const char *path) {
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(copy, 0755);
            *p = '/';
        }
    }
}

static void join(char *out, size_t size, const char *dir, const char *name) {
    snprintf(out, size, "%s/%s", dir, name);
}

/* ---- zip ---- */

static uint32_t get16(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t get32(const unsigned char *p) {
    return get16(p) | get16(p + 2) << 16;
}

static void put16(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

/* Inflates (method 8) or copies (method 0) `in_size` bytes from `in` to `out`. */
static bool unpack_entry(FILE *in, FILE *out, int method, uint32_t in_size) {
    static unsigned char inbuf[CHUNK], outbuf[CHUNK];
    if (method == 0) {
        while (in_size) {
            size_t n = fread(inbuf, 1, in_size < CHUNK ? in_size : CHUNK, in);
            if (!n || fwrite(inbuf, 1, n, out) != n) {
                return false;
            }
            in_size -= (uint32_t)n;
        }
        return true;
    }
    if (method != 8) {
        return false;
    }
    z_stream z = {0};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
        return false;
    }
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (!z.avail_in) {
            size_t n = fread(inbuf, 1, in_size < CHUNK ? in_size : CHUNK, in);
            if (!n) {
                break;
            }
            in_size -= (uint32_t)n;
            z.next_in = inbuf;
            z.avail_in = (uInt)n;
        }
        z.next_out = outbuf;
        z.avail_out = CHUNK;
        status = inflate(&z, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            break;
        }
        size_t made = CHUNK - z.avail_out;
        if (fwrite(outbuf, 1, made, out) != made) {
            break;
        }
    }
    inflateEnd(&z);
    return status == Z_STREAM_END;
}

static int zip_read(const char *path, const char *dir, bool list) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return fail("can't open", path);
    }
    /* The end record is in the last 64 KiB (after a comment, if any). */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tail = size < 65557 ? size : 65557;
    unsigned char *end = malloc((size_t)tail);
    fseek(f, size - tail, SEEK_SET);
    if (!end || fread(end, 1, (size_t)tail, f) != (size_t)tail) {
        fclose(f);
        return fail("can't read", path);
    }
    long at = -1;
    for (long i = tail - 22; i >= 0; i--) {
        if (get32(end + i) == 0x06054b50) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        free(end);
        fclose(f);
        return fail("not a zip file", path);
    }
    uint32_t entries = get16(end + at + 10), cd_offset = get32(end + at + 16);
    free(end);
    long next = cd_offset;
    for (uint32_t e = 0; e < entries; e++) {
        unsigned char h[46];
        fseek(f, next, SEEK_SET);
        if (fread(h, 1, 46, f) != 46 || get32(h) != 0x02014b50) {
            fclose(f);
            return fail("damaged zip file", path);
        }
        int method = (int)get16(h + 10);
        uint32_t packed = get32(h + 20), unpacked = get32(h + 24);
        uint32_t name_len = get16(h + 28), extra_len = get16(h + 30), comment_len = get16(h + 32);
        uint32_t local = get32(h + 42);
        char name[512] = "";
        if (name_len >= sizeof(name) || fread(name, 1, name_len, f) != name_len) {
            fclose(f);
            return fail("damaged zip file", path);
        }
        name[name_len] = '\0';
        next += 46 + name_len + extra_len + comment_len;
        bool folder = name_len && name[name_len - 1] == '/';
        if (list) {
            printf("%10u  %s\n", unpacked, name);
            continue;
        }
        if (!safe_name(name)) {
            fprintf(stderr, "archive: skipping %s\n", name);
            continue;
        }
        char out_path[1024];
        join(out_path, sizeof(out_path), dir, name);
        make_parents(out_path);
        if (folder) {
            mkdir(out_path, 0755);
            continue;
        }
        unsigned char lh[30];
        fseek(f, local, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || get32(lh) != 0x04034b50) {
            fclose(f);
            return fail("damaged zip file", path);
        }
        fseek(f, (long)local + 30 + get16(lh + 26) + get16(lh + 28), SEEK_SET);
        FILE *out = fopen(out_path, "wb");
        if (!out) {
            fclose(f);
            return fail("can't write", out_path);
        }
        bool ok = unpack_entry(f, out, method, packed);
        fclose(out);
        if (!ok) {
            fclose(f);
            return fail("can't unpack", name);
        }
        file_count++;
    }
    fclose(f);
    return 0;
}

/* The zip being written: its file, and its central directory so far. */
static FILE *zip_out;
static unsigned char *central;
static size_t central_size, central_room;
static uint32_t zip_entries;

static unsigned char *read_whole(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(n > 0 ? (size_t)n : 1);
    if (data && n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = n > 0 ? (size_t)n : 0;
    return data;
}

static bool zip_add(const char *path, const char *name, bool folder) {
    size_t size = 0;
    unsigned char *data = folder ? NULL : read_whole(path, &size);
    if (!folder && !data) {
        return false;
    }
    uint32_t crc = folder ? 0 : (uint32_t)crc32(0, data, (uInt)size);
    /* Deflated, unless that comes out bigger. */
    unsigned char *packed = NULL;
    size_t packed_size = size;
    int method = 0;
    if (size) {
        uLong room = compressBound((uLong)size) + 64;
        packed = malloc(room);
        z_stream z = {0};
        if (packed && deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
            z.next_in = data;
            z.avail_in = (uInt)size;
            z.next_out = packed;
            z.avail_out = (uInt)room;
            if (deflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out < size) {
                method = 8;
                packed_size = z.total_out;
            }
            deflateEnd(&z);
        }
    }
    size_t name_len = strlen(name);
    uint32_t offset = (uint32_t)ftell(zip_out);
    unsigned char h[30] = {0};
    put32(h, 0x04034b50);
    put16(h + 4, 20);
    put16(h + 8, (uint32_t)method);
    put32(h + 14, crc);
    put32(h + 18, (uint32_t)packed_size);
    put32(h + 22, (uint32_t)size);
    put16(h + 26, (uint32_t)name_len);
    fwrite(h, 1, 30, zip_out);
    fwrite(name, 1, name_len, zip_out);
    if (size) {
        fwrite(method ? packed : data, 1, packed_size, zip_out);
    }
    free(packed);
    free(data);
    if (central_size + 46 + name_len > central_room) {
        central_room = (central_room + 46 + name_len) * 2;
        central = realloc(central, central_room);
    }
    unsigned char *c = central + central_size;
    memset(c, 0, 46);
    put32(c, 0x02014b50);
    put16(c + 4, 20);
    put16(c + 6, 20);
    put16(c + 10, (uint32_t)method);
    put32(c + 16, crc);
    put32(c + 20, (uint32_t)packed_size);
    put32(c + 24, (uint32_t)size);
    put16(c + 28, (uint32_t)name_len);
    put32(c + 38, folder ? 0x10 : 0);
    put32(c + 42, offset);
    memcpy(c + 46, name, name_len);
    central_size += 46 + name_len;
    zip_entries++;
    if (!folder) {
        file_count++;
    }
    return true;
}

/* ---- tar (through gzip or not) ---- */

/* The tar being read or written, through zlib's gzip stream or plainly. */
static FILE *tar_file;
static bool tar_gzip;
static z_stream tar_z;
static unsigned char tar_buffer[CHUNK];

static size_t tar_read(void *out, size_t size) {
    if (!tar_gzip) {
        return fread(out, 1, size, tar_file);
    }
    tar_z.next_out = out;
    tar_z.avail_out = (uInt)size;
    while (tar_z.avail_out) {
        if (!tar_z.avail_in) {
            size_t n = fread(tar_buffer, 1, CHUNK, tar_file);
            if (!n) {
                break;
            }
            tar_z.next_in = tar_buffer;
            tar_z.avail_in = (uInt)n;
        }
        int status = inflate(&tar_z, Z_NO_FLUSH);
        if (status == Z_STREAM_END || (status != Z_OK && status != Z_BUF_ERROR)) {
            break;
        }
    }
    return size - tar_z.avail_out;
}

static void tar_write(const void *data, size_t size, bool finish) {
    if (!tar_gzip) {
        fwrite(data, 1, size, tar_file);
        return;
    }
    tar_z.next_in = (unsigned char *)data;
    tar_z.avail_in = (uInt)size;
    do {
        tar_z.next_out = tar_buffer;
        tar_z.avail_out = CHUNK;
        deflate(&tar_z, finish ? Z_FINISH : Z_NO_FLUSH);
        fwrite(tar_buffer, 1, CHUNK - tar_z.avail_out, tar_file);
    } while (tar_z.avail_out == 0 || (finish && tar_z.avail_in));
}

static unsigned long octal(const char *p, size_t n) {
    unsigned long v = 0;
    for (size_t i = 0; i < n && p[i]; i++) {
        if (p[i] >= '0' && p[i] <= '7') {
            v = v * 8 + (unsigned long)(p[i] - '0');
        }
    }
    return v;
}

static int tar_extract(const char *path, const char *dir, bool list) {
    static char block[512], long_name[1024];
    bool have_long_name = false;
    for (;;) {
        if (tar_read(block, 512) != 512) {
            break;
        }
        if (!block[0]) {
            break; /* The two empty blocks at the end. */
        }
        unsigned long size = octal(block + 124, 12);
        char type = block[156];
        char name[1024];
        if (have_long_name) {
            snprintf(name, sizeof(name), "%s", long_name);
            have_long_name = false;
        } else if (!memcmp(block + 257, "ustar", 5) && block[345]) {
            snprintf(name, sizeof(name), "%.155s/%.100s", block + 345, block);
        } else {
            snprintf(name, sizeof(name), "%.100s", block);
        }
        unsigned long blocks = (size + 511) / 512;
        if (type == 'L') { /* GNU: the next entry's long name. */
            size_t n = size < sizeof(long_name) - 1 ? size : sizeof(long_name) - 1;
            memset(long_name, 0, sizeof(long_name));
            for (unsigned long b = 0; b < blocks; b++) {
                tar_read(block, 512);
                if (b * 512 < n) {
                    memcpy(long_name + b * 512, block, n - b * 512 < 512 ? n - b * 512 : 512);
                }
            }
            have_long_name = true;
            continue;
        }
        bool folder = type == '5', file = type == '0' || type == '\0' || type == '7';
        if (list && (folder || file)) {
            printf("%10lu  %s%s\n", size, name, folder && !ends_with(name, "/") ? "/" : "");
        }
        FILE *out = NULL;
        if (!list && (folder || file) && safe_name(name)) {
            char out_path[1100];
            join(out_path, sizeof(out_path), dir, name);
            make_parents(out_path);
            if (folder) {
                mkdir(out_path, 0755);
            } else if ((out = fopen(out_path, "wb"))) {
                chmod(out_path, (mode_t)(octal(block + 100, 8) & 0777));
                file_count++;
            } else {
                return fail("can't write", out_path);
            }
        }
        for (unsigned long b = 0; b < blocks; b++) {
            if (tar_read(block, 512) != 512) {
                if (out) {
                    fclose(out);
                }
                return fail("damaged archive", path);
            }
            if (out) {
                unsigned long left = size - b * 512;
                fwrite(block, 1, left < 512 ? left : 512, out);
            }
        }
        if (out) {
            fclose(out);
        }
    }
    return 0;
}

/* A number in a tar header: `n - 1` octal digits and a NUL. */
static void put_octal(char *p, size_t n, unsigned long v) {
    p[n - 1] = '\0';
    for (size_t i = n - 1; i-- > 0; v >>= 3) {
        p[i] = (char)('0' + (v & 7));
    }
}

static void tar_header(const char *name, unsigned long size, unsigned mode, bool folder) {
    char block[512] = {0};
    size_t len = strlen(name);
    if (len > 99) { /* A GNU long name entry first. */
        char lh[512] = {0};
        snprintf(lh, 100, "././@LongLink");
        put_octal(lh + 100, 8, 0644);
        put_octal(lh + 124, 12, (unsigned long)len + 1);
        lh[156] = 'L';
        memcpy(lh + 257, "ustar  ", 8);
        memset(lh + 148, ' ', 8);
        unsigned sum = 0;
        for (int i = 0; i < 512; i++) {
            sum += (unsigned char)lh[i];
        }
        put_octal(lh + 148, 7, sum);
        tar_write(lh, 512, false);
        for (size_t at = 0; at <= len; at += 512) {
            char chunk[512] = {0};
            memcpy(chunk, name + at, len + 1 - at < 512 ? len + 1 - at : 512);
            tar_write(chunk, 512, false);
        }
    }
    memcpy(block, name, len < 99 ? len : 99);
    put_octal(block + 100, 8, mode & 0777);
    put_octal(block + 108, 8, 0);
    put_octal(block + 116, 8, 0);
    put_octal(block + 124, 12, folder ? 0 : size);
    put_octal(block + 136, 12, 0);
    block[156] = folder ? '5' : '0';
    memcpy(block + 257, "ustar  ", 8);
    memset(block + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) {
        sum += (unsigned char)block[i];
    }
    put_octal(block + 148, 7, sum);
    tar_write(block, 512, false);
}

static bool tar_add(const char *path, const char *name, bool folder) {
    struct stat st;
    unsigned mode = stat(path, &st) == 0 ? (unsigned)st.st_mode : 0644;
    if (folder) {
        char with_slash[1024];
        snprintf(with_slash, sizeof(with_slash), "%s/", name);
        tar_header(with_slash, 0, mode ? mode : 0755, true);
        return true;
    }
    FILE *in = fopen(path, "rb");
    if (!in) {
        return false;
    }
    fseek(in, 0, SEEK_END);
    unsigned long size = (unsigned long)ftell(in);
    fseek(in, 0, SEEK_SET);
    tar_header(name, size, mode, false);
    static unsigned char data[CHUNK];
    unsigned long done = 0;
    size_t n;
    while ((n = fread(data, 1, CHUNK, in)) > 0) {
        tar_write(data, n, false);
        done += n;
    }
    fclose(in);
    static const char zeros[512];
    if (done % 512) {
        tar_write(zeros, 512 - done % 512, false);
    }
    file_count++;
    return true;
}

/* ---- Packing ---- */

static bool (*add_entry)(const char *path, const char *name, bool folder);

/* Adds a file, or a folder and everything in it, under `name`. */
static bool add_tree(const char *path, const char *name) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "archive: can't find %s\n", path);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        return add_entry(path, name, false);
    }
    if (add_entry == zip_add) {
        char folder[1024];
        snprintf(folder, sizeof(folder), "%s/", name);
        add_entry(path, folder, true);
    } else {
        add_entry(path, name, true);
    }
    DIR *d = opendir(path);
    if (!d) {
        return false;
    }
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        char child[1024], child_name[1024];
        join(child, sizeof(child), path, e->d_name);
        join(child_name, sizeof(child_name), name, e->d_name);
        ok &= add_tree(child, child_name);
    }
    closedir(d);
    return ok;
}

static const char *base_name(const char *path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') {
        n--;
    }
    static char base[256];
    size_t start = n;
    while (start && path[start - 1] != '/') {
        start--;
    }
    snprintf(base, sizeof(base), "%.*s", (int)(n - start), path + start);
    return base;
}

static int create(const char *out, char **inputs, int count) {
    bool zip = ends_with(out, ".zip");
    bool gz = ends_with(out, ".tar.gz") || ends_with(out, ".tgz");
    if (!zip && !gz && !ends_with(out, ".tar")) {
        return fail("make a .zip, .tar.gz, .tgz or .tar file, not", out);
    }
    FILE *f = fopen(out, "wb");
    if (!f) {
        return fail("can't write", out);
    }
    bool ok = true;
    if (zip) {
        zip_out = f;
        add_entry = zip_add;
        for (int i = 0; i < count; i++) {
            ok &= add_tree(inputs[i], base_name(inputs[i]));
        }
        uint32_t cd_offset = (uint32_t)ftell(f);
        fwrite(central, 1, central_size, f);
        unsigned char e[22] = {0};
        put32(e, 0x06054b50);
        put16(e + 8, zip_entries);
        put16(e + 10, zip_entries);
        put32(e + 12, (uint32_t)central_size);
        put32(e + 16, cd_offset);
        fwrite(e, 1, 22, f);
    } else {
        tar_file = f;
        tar_gzip = gz;
        if (gz && deflateInit2(&tar_z, 6, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            fclose(f);
            return fail("can't compress", out);
        }
        add_entry = tar_add;
        for (int i = 0; i < count; i++) {
            ok &= add_tree(inputs[i], base_name(inputs[i]));
        }
        static const char end[1024];
        tar_write(end, sizeof(end), true);
        if (gz) {
            deflateEnd(&tar_z);
        }
    }
    fclose(f);
    printf("archive: made %s (%d file%s)\n", out, file_count, file_count == 1 ? "" : "s");
    return ok ? 0 : 1;
}

/* ---- Unpacking ---- */

/* A single gzipped file (not a tar): name.gz unpacks to name. */
static int gunzip(const char *path, const char *dir) {
    char out_path[1024];
    char name[256];
    snprintf(name, sizeof(name), "%s", base_name(path));
    name[strlen(name) - 3] = '\0';
    join(out_path, sizeof(out_path), dir, name);
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        return fail("can't write", out_path);
    }
    size_t n;
    static unsigned char data[CHUNK];
    while ((n = tar_read(data, CHUNK)) > 0) {
        fwrite(data, 1, n, out);
    }
    fclose(out);
    file_count++;
    return 0;
}

static int read_archive(const char *path, const char *dir, bool list) {
    if (ends_with(path, ".zip")) {
        return zip_read(path, dir, list);
    }
    bool gz = ends_with(path, ".gz") || ends_with(path, ".tgz");
    if (!gz && !ends_with(path, ".tar")) {
        return fail("not a .zip, .tar.gz, .tgz, .tar or .gz file", path);
    }
    tar_file = fopen(path, "rb");
    if (!tar_file) {
        return fail("can't open", path);
    }
    tar_gzip = gz;
    if (gz && inflateInit2(&tar_z, MAX_WBITS + 16) != Z_OK) {
        fclose(tar_file);
        return fail("can't unpack", path);
    }
    int result = gz && !ends_with(path, ".tar.gz") && !ends_with(path, ".tgz")
                     ? (list ? (printf("%s\n", base_name(path)), 0) : gunzip(path, dir))
                     : tar_extract(path, dir, list);
    if (gz) {
        inflateEnd(&tar_z);
    }
    fclose(tar_file);
    return result;
}

/* The folder an archive unpacks into by default: next to it, named after it
 * ("photos.zip": "photos", or "photos 2" if that's taken). */
static void default_folder(const char *path, char *out, size_t size) {
    char stem[1024];
    snprintf(stem, sizeof(stem), "%s", path);
    static const char *const endings[] = {".tar.gz", ".tgz", ".zip", ".tar", ".gz"};
    for (size_t i = 0; i < sizeof(endings) / sizeof(endings[0]); i++) {
        if (ends_with(stem, endings[i])) {
            stem[strlen(stem) - strlen(endings[i])] = '\0';
            break;
        }
    }
    struct stat st;
    snprintf(out, size, "%s", stem);
    for (int n = 2; stat(out, &st) == 0; n++) {
        snprintf(out, size, "%s %d", stem, n);
    }
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "list")) {
        return read_archive(argv[2], ".", true);
    }
    if (argc >= 3 && !strcmp(argv[1], "extract")) {
        char dir[1024];
        if (argc >= 4) {
            snprintf(dir, sizeof(dir), "%s", argv[3]);
        } else {
            default_folder(argv[2], dir, sizeof(dir));
        }
        make_parents(dir);
        mkdir(dir, 0755);
        int result = read_archive(argv[2], dir, false);
        if (!result) {
            printf("archive: extracted %d file%s to %s\n", file_count, file_count == 1 ? "" : "s",
                   dir);
        }
        return result;
    }
    if (argc >= 4 && !strcmp(argv[1], "create")) {
        return create(argv[2], argv + 3, argc - 3);
    }
    fprintf(stderr, "usage: archive list FILE\n"
                    "       archive extract FILE [FOLDER]\n"
                    "       archive create OUT.zip|OUT.tar.gz|OUT.tar FILE...\n");
    return 2;
}
