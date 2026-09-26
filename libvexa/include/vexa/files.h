#ifndef VEXA_FILES_H
#define VEXA_FILES_H

#include <stddef.h>

/*
 * Whole files and folders: what a file manager (or cp -r, rm -r) does.
 * Each returns 0 or a negative VX_E* error, and stops at the first error.
 */

/* Copies a file, a symbolic link, or a folder with everything in it.
 * `to` must not exist yet. */
long vx_copy_tree(const char *from, const char *to);
/* Removes a file, a link, or a folder with everything in it. */
long vx_remove_tree(const char *path);
/* Renames; across file systems, copies and then removes. */
long vx_move(const char *from, const char *to);
/* The size of a file, or of the files in a folder (and how many there are). */
unsigned long long vx_tree_size(const char *path, long *files);
/* A name for something new in `dir` that isn't taken: `name`, else
 * "stem 2.ext", "stem 3.ext"... ("Files 2.vxapp", "notes 2.txt"). */
void vx_unique_name(const char *dir, const char *name, char *out, size_t size);
/* "dir/name" (one slash). */
void vx_join_path(char *out, size_t size, const char *dir, const char *name);

#endif
