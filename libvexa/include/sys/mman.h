#ifndef LIBVEXA_SYS_MMAN_H
#define LIBVEXA_SYS_MMAN_H

/* Anonymous memory (and shared mappings of files and devices). */
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)

void *mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset);
int munmap(void *address, size_t size);
int mprotect(void *address, size_t size, int protection);

#ifdef __cplusplus
}
#endif

#endif
