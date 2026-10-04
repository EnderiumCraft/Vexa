#ifndef LIBVEXA_DIRENT_H
#define LIBVEXA_DIRENT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};

typedef struct __vx_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
void rewinddir(DIR *dir);
int dirfd(DIR *dir);
int scandir(const char *path, struct dirent ***list, int (*keep)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);

#ifdef __cplusplus
}
#endif

#endif
