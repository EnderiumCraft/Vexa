/* A small libdrm for Vexa: what Mesa's virgl driver uses (the ioctl wrapper
 * and the device's version), for /dev/dri/renderD128 (the virtio GPU). No
 * PRIME (sharing buffers between devices or processes) or modesetting. */
#ifndef XF86DRM_H
#define XF86DRM_H

#include <stddef.h>
#include <stdint.h>
#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_DIR_NAME "/dev/dri"
#define DRM_RENDER_MINOR_NAME "renderD"
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER 2

typedef struct _drmVersion {
    int version_major, version_minor, version_patchlevel;
    int name_len;
    char *name;
    int date_len;
    char *date;
    int desc_len;
    char *desc;
} drmVersion, *drmVersionPtr;

int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

#ifdef __cplusplus
}
#endif

#endif
