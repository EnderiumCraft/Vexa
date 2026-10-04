#ifndef VEXA_VIRTIO_GPU_H
#define VEXA_VIRTIO_GPU_H

#include <stdbool.h>
#include <stdint.h>

struct file;

/* The virtio GPU with 3D (virgl), if there is one: /dev/dri/renderD128. */
void virtio_gpu_init(void);

/* For the Linux subsystem: whether a file is the GPU, and an ioctl on it
 * (Linux's virtgpu DRM requests, with `arg` a user pointer). */
bool virtio_gpu_file(struct file *file);
int64_t virtio_gpu_ioctl(struct file *file, uint32_t request, uint64_t arg);

#endif
