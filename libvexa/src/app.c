#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/syscall.h>

/* Apps: .vxapp bundles (see <vexa/app.h>). */

#define MAX_APPS 64

static bool ends_with(const char *text, const char *suffix) {
    size_t n = strlen(text), m = strlen(suffix);
    return n >= m && !strcmp(text + n - m, suffix);
}

bool vx_app_is_bundle(const char *path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') {
        n--;
    }
    size_t m = strlen(VX_APP_EXTENSION);
    return n > m && !strncmp(path + n - m, VX_APP_EXTENSION, m);
}

static void copy(char *to, size_t size, const char *from) {
    strncpy(to, from, size - 1);
    to[size - 1] = '\0';
}

int vx_app_load(const char *bundle, struct vx_app *app) {
    memset(app, 0, sizeof(*app));
    copy(app->bundle, sizeof(app->bundle), bundle);
    size_t n = strlen(app->bundle);
    while (n > 1 && app->bundle[n - 1] == '/') {
        app->bundle[--n] = '\0';
    }
    char path[320];
    snprintf(path, sizeof(path), "%s/Contents/Info.conf", app->bundle);
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        return handle;
    }
    char text[2048];
    long got = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    text[got > 0 ? got : 0] = '\0';

    char executable[128] = "", icon[128] = "";
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
        if (!strcmp(line, "name")) {
            copy(app->name, sizeof(app->name), value);
        } else if (!strcmp(line, "executable")) {
            copy(executable, sizeof(executable), value);
        } else if (!strcmp(line, "icon")) {
            copy(icon, sizeof(icon), value);
        } else if (!strcmp(line, "opens")) {
            copy(app->opens, sizeof(app->opens), value);
        } else if (!strcmp(line, "shortcut")) {
            copy(app->shortcut, sizeof(app->shortcut), value);
        } else if (!strcmp(line, "menu")) {
            app->menu = atoi(value);
        } else if (!strcmp(line, "desktop")) {
            app->desktop = !strcmp(value, "yes");
        } else if (!strcmp(line, "kind")) {
            app->is_linux = !strcmp(value, "linux");
        }
    }
    if (!executable[0]) {
        return -VX_ENOEXEC;
    }
    if (executable[0] == '/') {
        copy(app->executable, sizeof(app->executable), executable);
    } else {
        snprintf(app->executable, sizeof(app->executable), "%s/Contents/Vexa/%s", app->bundle,
                 executable);
    }
    if (icon[0]) {
        snprintf(app->icon, sizeof(app->icon), "%s/Contents/Resources/%s", app->bundle, icon);
    }
    if (!app->name[0]) { /* The bundle's name, without ".vxapp". */
        const char *slash = strrchr(app->bundle, '/');
        copy(app->name, sizeof(app->name), slash ? slash + 1 : app->bundle);
        char *dot = strrchr(app->name, '.');
        if (dot) {
            *dot = '\0';
        }
    }
    return 0;
}

static int compare_apps(const void *a, const void *b) {
    const struct vx_app *x = a, *y = b;
    /* In the menu first, by place; then the rest by name. */
    if (x->menu != y->menu) {
        if (!x->menu || !y->menu) {
            return x->menu ? -1 : 1;
        }
        return x->menu < y->menu ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

int vx_app_list(struct vx_app *apps, int max) {
    int handle = vx_open(VX_APPS_DIR, VX_OPEN_READ);
    if (handle < 0) {
        return 0;
    }
    int count = 0;
    struct vx_dir_entry entries[16];
    long n;
    while ((n = vx_read_dir(handle, entries, 16)) > 0) {
        for (long i = 0; i < n && count < max; i++) {
            if (!ends_with(entries[i].name, VX_APP_EXTENSION)) {
                continue;
            }
            char bundle[256];
            snprintf(bundle, sizeof(bundle), "%s/%s", VX_APPS_DIR, entries[i].name);
            struct vx_stat stat;
            if (vx_app_load(bundle, &apps[count]) == 0 &&
                vx_stat(apps[count].executable, &stat) == 0) {
                count++; /* (Linux apps are left out without the Linux files.) */
            }
        }
    }
    vx_close(handle);
    qsort(apps, (size_t)count, sizeof(apps[0]), compare_apps);
    return count;
}

static bool same_name(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++, b++;
    }
    return !*a && !*b;
}

int vx_app_find(const char *name, struct vx_app *app) {
    if (strchr(name, '/')) { /* A path to a bundle. */
        return vx_app_load(name, app);
    }
    struct vx_app *apps = malloc(MAX_APPS * sizeof(*apps));
    if (!apps) {
        return -VX_ENOMEM;
    }
    int count = vx_app_list(apps, MAX_APPS), found = -VX_ENOENT;
    for (int i = 0; i < count && found; i++) {
        const char *file = strrchr(apps[i].bundle, '/') + 1;
        char stem[128];
        copy(stem, sizeof(stem), file);
        stem[strlen(stem) - strlen(VX_APP_EXTENSION)] = '\0';
        if (same_name(name, apps[i].name) || same_name(name, file) || same_name(name, stem)) {
            *app = apps[i];
            found = 0;
        }
    }
    free(apps);
    return found;
}

/* True if `list` ("png bmp ppm") has `word`. */
static bool has_word(const char *list, const char *word) {
    size_t n = strlen(word);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        size_t m = strcspn(p, " ,");
        if (m == n && m && !strncmp(p, word, n)) {
            return true;
        }
        p += m;
    }
    return false;
}

int vx_app_for_file(const char *path, struct vx_app *app) {
    const char *slash = strrchr(path, '/'), *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    char extension[32] = "";
    if (dot && dot != base && strlen(dot + 1) < sizeof(extension)) {
        for (int i = 0; dot[1 + i]; i++) {
            extension[i] = (char)tolower((unsigned char)dot[1 + i]);
        }
    }
    struct vx_app *apps = malloc(MAX_APPS * sizeof(*apps));
    if (!apps) {
        return -VX_ENOMEM;
    }
    int count = vx_app_list(apps, MAX_APPS), found = -1;
    for (int i = 0; i < count && found < 0 && extension[0]; i++) {
        if (has_word(apps[i].opens, extension)) {
            found = i;
        }
    }
    for (int i = 0; i < count && found < 0; i++) {
        if (has_word(apps[i].opens, "*")) {
            found = i;
        }
    }
    if (found >= 0) {
        *app = apps[found];
    }
    free(apps);
    return found >= 0 ? 0 : -VX_ENOENT;
}

int vx_app_open(const struct vx_app *app, const char *file) {
    const char *argv[] = {app->executable, file};
    unsigned long envc = 0;
    while (environ[envc]) {
        envc++;
    }
    struct vx_spawn spawn = {
        .argv = argv, .argc = file ? 2 : 1, .envp = (const char *const *)environ, .envc = envc,
        .handles = {0, 1, 2}, .flags = VX_SPAWN_NEW_GROUP,
    };
    return vx_spawn(app->executable, &spawn);
}
