/* A small libdrm for Vexa (see xf86drm.h). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include "xf86drm.h"

int drmIoctl(int fd, unsigned long request, void *arg) {
    int result;
    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && (errno == EINTR || errno == EAGAIN));
    return result;
}

drmVersionPtr drmGetVersion(int fd) {
    char name[64] = "", date[64] = "", desc[128] = "";
    struct drm_version v = {
        .name_len = sizeof(name) - 1, .name = name,
        .date_len = sizeof(date) - 1, .date = date,
        .desc_len = sizeof(desc) - 1, .desc = desc,
    };
    if (drmIoctl(fd, DRM_IOCTL_VERSION, &v) != 0) {
        return NULL;
    }
    drmVersionPtr version = calloc(1, sizeof(*version));
    if (!version) {
        return NULL;
    }
    version->version_major = v.version_major;
    version->version_minor = v.version_minor;
    version->version_patchlevel = v.version_patchlevel;
    version->name = strdup(name);
    version->name_len = (int)strlen(name);
    version->date = strdup(date);
    version->date_len = (int)strlen(date);
    version->desc = strdup(desc);
    version->desc_len = (int)strlen(desc);
    return version;
}

void drmFreeVersion(drmVersionPtr version) {
    if (version) {
        free(version->name);
        free(version->date);
        free(version->desc);
        free(version);
    }
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
    (void)fd, (void)handle, (void)flags, (void)prime_fd;
    errno = ENOSYS;
    return -1;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) {
    (void)fd, (void)prime_fd, (void)handle;
    errno = ENOSYS;
    return -1;
}
