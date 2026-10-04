#ifndef VEXA_SETTINGS_H
#define VEXA_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Settings: "key=value" files in /etc (desktop.conf, apps.conf...), and
 * each account's own in VX_SETTINGS_OWN_DIR of their home folder: loading
 * reads /etc's, then theirs over it; saving (by anyone but root) writes
 * theirs.
 *
 *     struct vx_settings s;
 *     vx_settings_load(&s, "desktop.conf");
 *     int hours = vx_settings_int(&s, "clock", 24);
 *     vx_settings_set(&s, "clock", "12");
 *     vx_settings_save(&s);
 *
 * /etc is in memory. When there's a disk to keep them on (the first
 * writable ext2 file system), saved settings are also copied to its
 * VX_SETTINGS_DIR, and vinit puts them back in /etc at the next boot.
 */

#define VX_SETTINGS_MAX 64
#define VX_SETTINGS_DIR ".vexa/etc" /* On the disk that keeps them. */
#define VX_SETTINGS_OWN_DIR ".config/vexa" /* In a home folder. */

struct vx_settings {
    char name[32]; /* The file in /etc. */
    int count;
    struct {
        char key[32];
        char value[224];
    } entries[VX_SETTINGS_MAX];
};

/* Reads /etc/<name>, then the account's own (a file that isn't there is
 * empty settings): 0, or a negative error for a name that isn't allowed. */
int vx_settings_load(struct vx_settings *s, const char *name);
/* A value, or `fallback` if the key isn't set. */
const char *vx_settings_get(const struct vx_settings *s, const char *key, const char *fallback);
int vx_settings_int(const struct vx_settings *s, const char *key, int fallback);
/* "yes"/"no" (also "1"/"0", "true"/"false", "on"/"off"). */
bool vx_settings_bool(const struct vx_settings *s, const char *key, bool fallback);
void vx_settings_set(struct vx_settings *s, const char *key, const char *value);
void vx_settings_set_int(struct vx_settings *s, const char *key, int value);
void vx_settings_set_bool(struct vx_settings *s, const char *key, bool value);
void vx_settings_unset(struct vx_settings *s, const char *key);
/* Writes the account's own settings file, or for root /etc/<name> (and
 * the copy on disk): 0 or a negative error. */
int vx_settings_save(const struct vx_settings *s);

/* Writes a whole file in /etc (and on the disk that keeps settings). */
int vx_settings_write_file(const char *name, const char *text, size_t length);
/* Where settings are kept on disk ("/mnt/vda1"), or false if nowhere: then
 * they last until the machine restarts. */
bool vx_settings_disk(char *out, size_t size);
/* At boot (vinit): copies what's on the disk back to /etc; returns how many files. */
int vx_settings_restore(void);


#ifdef __cplusplus
}
#endif

#endif
