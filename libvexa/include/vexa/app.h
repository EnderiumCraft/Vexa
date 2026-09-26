#ifndef VEXA_APP_H
#define VEXA_APP_H

#include <stdbool.h>

/*
 * Apps: bundles, as on macOS. An app is a folder named "Name.vxapp",
 * normally in /apps:
 *
 *     Files.vxapp/
 *         Contents/
 *             Info.conf           what the app is ("key=value" lines)
 *             Vexa/files          its program
 *             Resources/icon.png  its icon (48x48, with transparency)
 *
 * Info.conf:
 *
 *     name=Files              the name people see
 *     executable=files        in Contents/Vexa, or an absolute path
 *     icon=icon.png           in Contents/Resources
 *     opens=png bmp ppm       file name extensions it opens ("*": anything)
 *     shortcut=Ctrl+Alt+F     a desktop keyboard shortcut (a letter)
 *     menu=2                  its place in the desktop's menu (none: not there)
 *     desktop=yes             an icon on the desktop
 *     kind=linux              a Linux program (listed with them in the menu)
 *
 * Programs meant for the command line stay in /bin; the apps' programs
 * are linked there too (/bin/files is /apps/Files.vxapp/Contents/Vexa/files).
 */

#define VX_APPS_DIR "/apps"
#define VX_APP_EXTENSION ".vxapp"

struct vx_app {
    char bundle[256];     /* "/apps/Files.vxapp" */
    char name[48];
    char executable[320]; /* A full path. */
    char icon[320];       /* A full path, or "". */
    char opens[128];
    char shortcut[24];
    int menu;             /* 0: not in the menu. */
    bool desktop;
    bool is_linux;
};

/* True if `path` names a bundle (it ends in ".vxapp"). */
bool vx_app_is_bundle(const char *path);
/* Reads a bundle's Info.conf: 0, or a negative error. */
int vx_app_load(const char *bundle, struct vx_app *app);
/* The apps in /apps whose programs are there, by menu place, then name. */
int vx_app_list(struct vx_app *apps, int max);
/* An app in /apps by its name ("Text Editor") or its bundle's ("Edit",
 * "Edit.vxapp"): 0, or a negative error. */
int vx_app_find(const char *name, struct vx_app *app);
/* The app that opens a file: the first that lists its extension, else the
 * first that opens anything. 0, or a negative error. */
int vx_app_for_file(const char *path, struct vx_app *app);
/* Starts an app, with a file to open (or NULL): a process handle, or a
 * negative error. */
int vx_app_open(const struct vx_app *app, const char *file);

#endif
