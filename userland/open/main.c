/* open: opens files, folders and apps as a double click in Files would,
 * like macOS's open(1):
 *
 *     open photo.png          with the app for its type (Image Viewer)
 *     open /share             a folder, in Files
 *     open /apps/Files.vxapp  starts an app
 *     open -a Editor notes    with a given app (by name or bundle name)
 *     open -a Terminal        just starts it
 */
#include <stdio.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/syscall.h>

static int start(const struct vx_app *app, const char *file) {
    int process = vx_app_open(app, file);
    if (process < 0) {
        fprintf(stderr, "open: can't start %s: %s\n", app->name, vx_strerror(process));
        return 1;
    }
    vx_close(process); /* It runs on its own. */
    return 0;
}

static int open_path(const char *path) {
    struct vx_app app;
    struct vx_stat stat;
    int error = vx_stat(path, &stat);
    if (error) {
        fprintf(stderr, "open: %s: %s\n", path, vx_strerror(error));
        return 1;
    }
    if (vx_app_is_bundle(path)) {
        if ((error = vx_app_load(path, &app)) < 0) {
            fprintf(stderr, "open: %s is not an app: %s\n", path, vx_strerror(error));
            return 1;
        }
        return start(&app, NULL);
    }
    if (stat.type == VX_TYPE_DIRECTORY) {
        if (vx_app_find("Files", &app) < 0) {
            fprintf(stderr, "open: no Files app to show %s\n", path);
            return 1;
        }
        return start(&app, path);
    }
    if (vx_app_for_file(path, &app) < 0) {
        fprintf(stderr, "open: no app opens %s\n", path);
        return 1;
    }
    return start(&app, path);
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "usage: open FILE...\n       open -a APP [FILE...]\n");
        return argc < 2 ? 1 : 0;
    }
    int failed = 0;
    if (!strcmp(argv[1], "-a")) {
        struct vx_app app;
        if (argc < 3 || vx_app_find(argv[2], &app) < 0) {
            fprintf(stderr, "open: no app named %s\n", argc < 3 ? "" : argv[2]);
            return 1;
        }
        if (argc == 3) {
            return start(&app, NULL);
        }
        for (int i = 3; i < argc; i++) {
            failed |= start(&app, argv[i]);
        }
        return failed;
    }
    for (int i = 1; i < argc; i++) {
        failed |= open_path(argv[i]);
    }
    return failed;
}
