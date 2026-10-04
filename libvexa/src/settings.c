#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/files.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

/* Settings files in /etc, and each account's own over them (see
 * <vexa/settings.h>). */

static bool name_ok(const char *name) {
    return name[0] && name[0] != '.' && !strchr(name, '/') && strlen(name) < 32;
}

static void copy_text(char *out, size_t size, const char *in) {
    size_t n = strlen(in);
    n = n < size - 1 ? n : size - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

/* Someone's own settings: "$HOME/.config/vexa/<name>" (false for root,
 * whose are /etc's). */
static bool own_path(char *out, size_t size, const char *name) {
    struct vx_credentials me = {0};
    vx_credentials(NULL, &me);
    if (me.uid == 0) {
        return false;
    }
    char rel[64];
    snprintf(rel, sizeof(rel), "%s/%s", VX_SETTINGS_OWN_DIR, name);
    vx_home_path(out, size, rel);
    return true;
}

static void read_into(struct vx_settings *s, const char *path) {
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        return; /* No file yet: nothing set. */
    }
    static char text[16384];
    long n = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    text[n > 0 ? n : 0] = '\0';
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *value = strchr(line, '=');
        if (line[0] == '#' || !value) {
            continue;
        }
        *value++ = '\0';
        vx_settings_set(s, line, value);
    }
}

int vx_settings_load(struct vx_settings *s, const char *name) {
    memset(s, 0, sizeof(*s));
    if (!name_ok(name)) {
        return -VX_EINVAL;
    }
    copy_text(s->name, sizeof(s->name), name);
    char path[300];
    snprintf(path, sizeof(path), "/etc/%s", name);
    read_into(s, path);
    if (own_path(path, sizeof(path), name)) {
        read_into(s, path); /* Theirs win. */
    }
    return 0;
}

static int find(const struct vx_settings *s, const char *key) {
    for (int i = 0; i < s->count; i++) {
        if (!strcmp(s->entries[i].key, key)) {
            return i;
        }
    }
    return -1;
}

const char *vx_settings_get(const struct vx_settings *s, const char *key, const char *fallback) {
    int i = find(s, key);
    return i >= 0 ? s->entries[i].value : fallback;
}

int vx_settings_int(const struct vx_settings *s, const char *key, int fallback) {
    const char *value = vx_settings_get(s, key, NULL);
    return value && value[0] ? atoi(value) : fallback;
}

bool vx_settings_bool(const struct vx_settings *s, const char *key, bool fallback) {
    const char *v = vx_settings_get(s, key, NULL);
    if (!v) {
        return fallback;
    }
    if (!strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on")) {
        return true;
    }
    if (!strcmp(v, "no") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off")) {
        return false;
    }
    return fallback;
}

void vx_settings_set(struct vx_settings *s, const char *key, const char *value) {
    if (!key[0] || strchr(key, '=') || strchr(key, '\n')) {
        return;
    }
    int i = find(s, key);
    if (i < 0) {
        if (s->count == VX_SETTINGS_MAX) {
            return;
        }
        i = s->count++;
        copy_text(s->entries[i].key, sizeof(s->entries[i].key), key);
    }
    copy_text(s->entries[i].value, sizeof(s->entries[i].value), value);
    for (char *c = s->entries[i].value; *c; c++) {
        if (*c == '\n') {
            *c = ' ';
        }
    }
}

void vx_settings_set_int(struct vx_settings *s, const char *key, int value) {
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    vx_settings_set(s, key, text);
}

void vx_settings_set_bool(struct vx_settings *s, const char *key, bool value) {
    vx_settings_set(s, key, value ? "yes" : "no");
}

void vx_settings_unset(struct vx_settings *s, const char *key) {
    int i = find(s, key);
    if (i >= 0) {
        memmove(&s->entries[i], &s->entries[i + 1], (size_t)(s->count - i - 1) * sizeof(s->entries[0]));
        s->count--;
    }
}

bool vx_settings_disk(char *out, size_t size) {
    struct vx_mount_info mounts[16];
    long n = vx_mounts(mounts, 16);
    for (long i = 0; i < n && i < 16; i++) {
        if (!strcmp(mounts[i].path, "/") && !strcmp(mounts[i].type, "ext2")) {
            return false; /* An installed Vexa: /etc and /home are on disk already. */
        }
    }
    for (long i = 0; i < n && i < 16; i++) {
        if (!strcmp(mounts[i].type, "ext2") && !mounts[i].read_only) {
            copy_text(out, size, mounts[i].path);
            return true;
        }
    }
    return false;
}

static int write_whole(const char *path, const char *text, size_t length) {
    int handle = vx_open(path, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle < 0) {
        return handle;
    }
    long written = vx_write(handle, text, length);
    vx_close(handle);
    return written == (long)length ? 0 : written < 0 ? (int)written : -VX_EIO;
}

int vx_settings_write_file(const char *name, const char *text, size_t length) {
    if (!name_ok(name)) {
        return -VX_EINVAL;
    }
    char path[300];
    snprintf(path, sizeof(path), "/etc/%s", name);
    int error = write_whole(path, text, length);
    char disk[128];
    if (!error && vx_settings_disk(disk, sizeof(disk))) {
        snprintf(path, sizeof(path), "%s/.vexa", disk);
        vx_mkdir(path);
        snprintf(path, sizeof(path), "%s/%s", disk, VX_SETTINGS_DIR);
        vx_mkdir(path);
        snprintf(path, sizeof(path), "%s/%s/%s", disk, VX_SETTINGS_DIR, name);
        write_whole(path, text, length); /* A disk that fails is only a lost copy. */
    }
    return error;
}

int vx_settings_save(const struct vx_settings *s) {
    static char text[16384];
    size_t n = 0;
    for (int i = 0; i < s->count && n < sizeof(text) - 300; i++) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%s=%s\n", s->entries[i].key,
                              s->entries[i].value);
    }
    char path[300];
    if (own_path(path, sizeof(path), s->name)) {
        /* Someone's own: in their home folder (which keeps itself). */
        char dir[300];
        vx_home_path(dir, sizeof(dir), ".config");
        vx_mkdir(dir);
        vx_home_path(dir, sizeof(dir), VX_SETTINGS_OWN_DIR);
        vx_mkdir(dir);
        return write_whole(path, text, n);
    }
    return vx_settings_write_file(s->name, text, n);
}

int vx_settings_restore(void) {
    char disk[128], dir[300];
    if (!vx_settings_disk(disk, sizeof(disk))) {
        return 0;
    }
    snprintf(dir, sizeof(dir), "%s/%s", disk, VX_SETTINGS_DIR);
    int handle = vx_open(dir, VX_OPEN_READ);
    if (handle < 0) {
        return 0;
    }
    int restored = 0;
    struct vx_dir_entry entries[16];
    long n;
    while ((n = vx_read_dir(handle, entries, 16)) > 0) {
        for (long i = 0; i < n; i++) {
            if (entries[i].type != VX_TYPE_FILE || !name_ok(entries[i].name)) {
                continue;
            }
            char from[400], to[64];
            snprintf(from, sizeof(from), "%s/%s", dir, entries[i].name);
            snprintf(to, sizeof(to), "/etc/%s", entries[i].name);
            vx_remove(to);
            restored += vx_copy_tree(from, to) == 0;
        }
    }
    vx_close(handle);
    return restored;
}
