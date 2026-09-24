#ifndef VEXA_STORAGE_H
#define VEXA_STORAGE_H

/* Finds storage controllers and disks, and mounts the file systems on them.
 * Runs in the init thread (it may wait for devices). */
void storage_init(void);

#endif
