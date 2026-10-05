/*
 * pkg: installs, updates and removes apps from a package index.
 *
 *     pkg update               reads the index again
 *     pkg list                 every package (and which are installed)
 *     pkg search WORDS         packages whose name or summary has them
 *     pkg info NAME            all about one
 *     pkg install NAME...      downloads, checks and installs into /apps
 *     pkg remove NAME...       removes what pkg installed
 *     pkg upgrade              installs newer versions of what's installed
 *
 * The index (index.conf) is "key=value" lines in sections, one per package:
 *
 *     [hello]
 *     name=Hello SDK
 *     version=1.0
 *     category=Accessories
 *     summary=A window that says hello
 *     bundle=HelloSDK.vxapp       what the zip holds
 *     file=HelloSDK.vxapp.zip     next to the index (or a full URL)
 *     size=24801
 *     sha256=...                  the zip's; pkg won't install it otherwise
 *
 * It comes from /etc/pkg.conf's source= (or --source URL). fetch downloads,
 * archive unpacks. An installed app has Contents/Package.conf (its package and
 * version) in its bundle; apps without one came with Vexa and stay.
 */

#include <dirent.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vexa/app.h>
#include <vexa/files.h>
#include <vexa/hash.h>

#define CONFIG "/etc/pkg.conf"
#define DEFAULT_SOURCE "https://github.com/EnderiumCraft/Vexa/releases/download/packages/index.conf"
#define INDEX_CACHE "/tmp/pkg-index.conf"
#define MAX_PACKAGES 128

extern char **environ;

struct package {
    char id[48], name[64], version[24], category[24], summary[160];
    char bundle[96], file[256], sha256[65];
    long size;
    char installed[24]; /* Its installed version, or "". */
};

static struct package packages[MAX_PACKAGES];
static int package_count;
static char source[512];

/* Runs a program and waits for it: its exit status, or -1. */
static int run(const char *const argv[]) {
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

static void read_source(void) {
    snprintf(source, sizeof(source), "%s", DEFAULT_SOURCE);
    FILE *f = fopen(CONFIG, "r");
    if (!f) {
        return;
    }
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!strncmp(line, "source=", 7) && line[7]) {
            snprintf(source, sizeof(source), "%s", line + 7);
        }
    }
    fclose(f);
}

static int download(const char *url, const char *to) {
    const char *argv[] = {"/bin/fetch", "-o", to, url, NULL};
    return run(argv);
}

static int update(void) {
    printf("pkg: reading %s\n", source);
    fflush(stdout);
    if (download(source, INDEX_CACHE) != 0) {
        fprintf(stderr, "pkg: can't download the index\n");
        return 1;
    }
    return 0;
}

/* What's installed: Package.conf in each bundle in /apps. */
static void find_installed(void) {
    DIR *d = opendir(VX_APPS_DIR);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        char path[512], line[128], id[48] = "", version[24] = "";
        snprintf(path, sizeof(path), "%s/%s/Contents/Package.conf", VX_APPS_DIR, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        while (fgets(line, sizeof(line), f)) {
            trim(line);
            if (!strncmp(line, "package=", 8)) {
                snprintf(id, sizeof(id), "%s", line + 8);
            } else if (!strncmp(line, "version=", 8)) {
                snprintf(version, sizeof(version), "%s", line + 8);
            }
        }
        fclose(f);
        for (int i = 0; i < package_count; i++) {
            if (!strcmp(packages[i].id, id)) {
                snprintf(packages[i].installed, sizeof(packages[i].installed), "%s",
                         version[0] ? version : "?");
            }
        }
    }
    closedir(d);
}

static int load_index(void) {
    FILE *f = fopen(INDEX_CACHE, "r");
    if (!f) {
        if (update() != 0) {
            return 1;
        }
        f = fopen(INDEX_CACHE, "r");
        if (!f) {
            return 1;
        }
    }
    char line[600];
    struct package *p = NULL;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end || package_count == MAX_PACKAGES) {
                p = NULL;
                continue;
            }
            *end = '\0';
            p = &packages[package_count++];
            memset(p, 0, sizeof(*p));
            snprintf(p->id, sizeof(p->id), "%s", line + 1);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!p || !eq || line[0] == '#') {
            continue;
        }
        *eq = '\0';
        const char *key = line, *value = eq + 1;
        if (!strcmp(key, "name")) {
            snprintf(p->name, sizeof(p->name), "%s", value);
        } else if (!strcmp(key, "version")) {
            snprintf(p->version, sizeof(p->version), "%s", value);
        } else if (!strcmp(key, "category")) {
            snprintf(p->category, sizeof(p->category), "%s", value);
        } else if (!strcmp(key, "summary")) {
            snprintf(p->summary, sizeof(p->summary), "%s", value);
        } else if (!strcmp(key, "bundle")) {
            snprintf(p->bundle, sizeof(p->bundle), "%s", value);
        } else if (!strcmp(key, "file")) {
            snprintf(p->file, sizeof(p->file), "%s", value);
        } else if (!strcmp(key, "sha256")) {
            snprintf(p->sha256, sizeof(p->sha256), "%s", value);
        } else if (!strcmp(key, "size")) {
            p->size = atol(value);
        }
    }
    fclose(f);
    find_installed();
    return 0;
}

static struct package *find(const char *id) {
    for (int i = 0; i < package_count; i++) {
        if (!strcasecmp(packages[i].id, id) || !strcasecmp(packages[i].name, id)) {
            return &packages[i];
        }
    }
    return NULL;
}

static void show(const struct package *p) {
    char installed[40] = "";
    if (p->installed[0]) {
        snprintf(installed, sizeof(installed), "  [installed %s]", p->installed);
    }
    printf("%-16s %-10s %s%s\n", p->id, p->version, p->summary, installed);
}

static bool sha256_matches(const char *path, const char *expected) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    struct vx_sha256 h;
    vx_sha256_init(&h);
    static unsigned char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        vx_sha256_add(&h, buffer, n);
    }
    fclose(f);
    unsigned char digest[32];
    vx_sha256_end(&h, digest);
    char text[65];
    for (int i = 0; i < 32; i++) {
        snprintf(text + i * 2, 3, "%02x", digest[i]);
    }
    return !strcasecmp(text, expected);
}

static int install(struct package *p) {
    if (!p->bundle[0] || !p->file[0] || strlen(p->sha256) != 64 || strchr(p->bundle, '/')) {
        fprintf(stderr, "pkg: %s isn't complete in the index\n", p->id);
        return 1;
    }
    char url[800], zip[128];
    if (strstr(p->file, "://")) {
        snprintf(url, sizeof(url), "%s", p->file);
    } else {
        /* Next to the index. */
        snprintf(url, sizeof(url), "%s", source);
        char *slash = strrchr(url, '/');
        snprintf(slash ? slash + 1 : url, sizeof(url) - (size_t)(slash ? slash + 1 - url : 0), "%s",
                 p->file);
    }
    snprintf(zip, sizeof(zip), "/tmp/pkg-%s.zip", p->id);
    printf("pkg: downloading %s %s\n", p->name, p->version);
    fflush(stdout);
    if (download(url, zip) != 0) {
        fprintf(stderr, "pkg: can't download %s\n", url);
        return 1;
    }
    if (!sha256_matches(zip, p->sha256)) {
        fprintf(stderr, "pkg: %s isn't what the index says (SHA-256); not installing it\n", p->file);
        remove(zip);
        return 1;
    }
    /* Unpacked next to /apps first, then moved in: a half-unpacked app
     * never shows up. */
    char staging[128], unpacked[256], bundle[256];
    snprintf(staging, sizeof(staging), "%s/.pkg-%s", VX_APPS_DIR, p->id);
    vx_remove_tree(staging);
    const char *argv[] = {"/bin/archive", "extract", zip, staging, NULL};
    if (run(argv) != 0) {
        fprintf(stderr, "pkg: can't unpack %s (can you write to %s?)\n", zip, VX_APPS_DIR);
        vx_remove_tree(staging);
        remove(zip);
        return 1;
    }
    remove(zip);
    snprintf(unpacked, sizeof(unpacked), "%s/%s", staging, p->bundle);
    snprintf(bundle, sizeof(bundle), "%s/%s", VX_APPS_DIR, p->bundle);
    struct stat st;
    if (stat(unpacked, &st) != 0) {
        fprintf(stderr, "pkg: the zip has no %s\n", p->bundle);
        vx_remove_tree(staging);
        return 1;
    }
    vx_remove_tree(bundle); /* An older version. */
    if (rename(unpacked, bundle) != 0) {
        fprintf(stderr, "pkg: can't put %s in %s\n", p->bundle, VX_APPS_DIR);
        vx_remove_tree(staging);
        return 1;
    }
    vx_remove_tree(staging);
    char record[320];
    snprintf(record, sizeof(record), "%s/Contents/Package.conf", bundle);
    FILE *f = fopen(record, "w");
    if (f) {
        fprintf(f, "package=%s\nversion=%s\n", p->id, p->version);
        fclose(f);
    }
    printf("pkg: installed %s %s\n", p->name, p->version);
    snprintf(p->installed, sizeof(p->installed), "%s", p->version);
    return 0;
}

static int uninstall(struct package *p) {
    DIR *d = opendir(VX_APPS_DIR);
    int result = 1;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char path[512], line[128];
        snprintf(path, sizeof(path), "%s/%s/Contents/Package.conf", VX_APPS_DIR, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        bool ours = false;
        while (fgets(line, sizeof(line), f)) {
            trim(line);
            ours |= !strncmp(line, "package=", 8) && !strcmp(line + 8, p->id);
        }
        fclose(f);
        if (ours) {
            char bundle[512];
            snprintf(bundle, sizeof(bundle), "%s/%s", VX_APPS_DIR, e->d_name);
            if (vx_remove_tree(bundle) == 0) {
                printf("pkg: removed %s\n", p->name);
                result = 0;
            } else {
                fprintf(stderr, "pkg: can't remove %s\n", bundle);
            }
        }
    }
    if (d) {
        closedir(d);
    }
    if (result) {
        fprintf(stderr, "pkg: %s isn't installed\n", p->id);
    }
    return result;
}

static int usage(void) {
    fprintf(stderr, "usage: pkg [--source URL] update | list | search WORDS | info NAME |\n"
                    "           install NAME... | remove NAME... | upgrade\n");
    return 2;
}

int main(int argc, char **argv) {
    read_source();
    int first = 1;
    if (argc > 2 && !strcmp(argv[1], "--source")) {
        snprintf(source, sizeof(source), "%s", argv[2]);
        first = 3;
    }
    if (first >= argc) {
        return usage();
    }
    const char *command = argv[first];
    char **names = argv + first + 1;
    int name_count = argc - first - 1;
    if (!strcmp(command, "update")) {
        if (update() != 0 || load_index() != 0) {
            return 1;
        }
        printf("pkg: %d package%s\n", package_count, package_count == 1 ? "" : "s");
        return 0;
    }
    if (load_index() != 0) {
        return 1;
    }
    if (!strcmp(command, "list")) {
        for (int i = 0; i < package_count; i++) {
            show(&packages[i]);
        }
        return 0;
    }
    if (!strcmp(command, "search") && name_count) {
        for (int i = 0; i < package_count; i++) {
            bool all = true;
            for (int w = 0; w < name_count; w++) {
                all &= strcasestr(packages[i].name, names[w]) || strcasestr(packages[i].id, names[w]) ||
                       strcasestr(packages[i].summary, names[w]);
            }
            if (all) {
                show(&packages[i]);
            }
        }
        return 0;
    }
    if (!strcmp(command, "info") && name_count) {
        struct package *p = find(names[0]);
        if (!p) {
            fprintf(stderr, "pkg: no package %s\n", names[0]);
            return 1;
        }
        printf("%s (%s)\nversion: %s\ncategory: %s\nsize: %ld bytes\ninstalled: %s\n%s\n", p->name,
               p->id, p->version, p->category[0] ? p->category : "Other", p->size,
               p->installed[0] ? p->installed : "no", p->summary);
        return 0;
    }
    if ((!strcmp(command, "install") || !strcmp(command, "remove")) && name_count) {
        int failed = 0;
        for (int i = 0; i < name_count; i++) {
            struct package *p = find(names[i]);
            if (!p) {
                fprintf(stderr, "pkg: no package %s\n", names[i]);
                failed++;
            } else {
                failed += command[0] == 'i' ? install(p) : uninstall(p);
            }
        }
        return failed ? 1 : 0;
    }
    if (!strcmp(command, "upgrade")) {
        int failed = 0, done = 0;
        for (int i = 0; i < package_count; i++) {
            if (packages[i].installed[0] && strcmp(packages[i].installed, packages[i].version)) {
                failed += install(&packages[i]);
                done++;
            }
        }
        if (!done) {
            printf("pkg: everything is up to date\n");
        }
        return failed ? 1 : 0;
    }
    return usage();
}
