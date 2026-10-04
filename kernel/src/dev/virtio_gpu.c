#include <vexa/abi.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>
#include <vexa/vfs.h>
#include <vexa/virtio.h>
#include <vexa/virtio_gpu.h>

/*
 * virtio GPU with 3D (virgl): QEMU's virtio-gpu-gl, which runs OpenGL on the
 * host's GPU for the guest. Programs get it as /dev/dri/renderD128, with
 * Linux's virtgpu DRM interface (the requests Mesa's virgl driver makes):
 * each open file is a 3D context; resources are the host's textures and
 * buffers, each with guest pages behind it that the program maps (to fill,
 * or to read what was transferred back); command buffers go to the host as
 * they are.
 *
 * One request at a time on the control queue, and those that start work on
 * the host (command buffers, transfers) carry a fence, which the device
 * answers only when the work is done: so when a request returns, its work
 * is finished, and nothing is ever busy (WAIT has nothing to wait for).
 *
 * The display stays on the boot framebuffer: this is for drawing, not for
 * showing (no scanouts are set up).
 */

#define VIRTIO_GPU_MODERN 0x1050
#define GPU_FEATURE_VIRGL 0

/* Commands and responses (virtio spec 5.7.6). */
#define CMD_GET_CAPSET_INFO 0x0108
#define CMD_GET_CAPSET 0x0109
#define CMD_RESOURCE_UNREF 0x0102
#define CMD_RESOURCE_ATTACH_BACKING 0x0106
#define CMD_RESOURCE_DETACH_BACKING 0x0107
#define CMD_CTX_CREATE 0x0200
#define CMD_CTX_DESTROY 0x0201
#define CMD_CTX_ATTACH_RESOURCE 0x0202
#define CMD_CTX_DETACH_RESOURCE 0x0203
#define CMD_RESOURCE_CREATE_3D 0x0204
#define CMD_TRANSFER_TO_HOST_3D 0x0205
#define CMD_TRANSFER_FROM_HOST_3D 0x0206
#define CMD_SUBMIT_3D 0x0207
#define RESP_OK_NODATA 0x1100
#define RESP_OK_CAPSET_INFO 0x1102
#define RESP_OK_CAPSET 0x1103
#define FLAG_FENCE 1

struct __attribute__((packed)) gpu_header {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_index, padding[3];
};

struct __attribute__((packed)) gpu_box {
    uint32_t x, y, z, w, h, d;
};

struct __attribute__((packed)) gpu_resource_create_3d {
    struct gpu_header header;
    uint32_t resource_id, target, format, bind, width, height, depth, array_size, last_level,
        nr_samples, flags, padding;
};

struct __attribute__((packed)) gpu_attach_backing {
    struct gpu_header header;
    uint32_t resource_id, entry_count;
    struct __attribute__((packed)) {
        uint64_t address;
        uint32_t length, padding;
    } entries[];
};

struct __attribute__((packed)) gpu_resource_command { /* UNREF, DETACH_BACKING, CTX_(DE)ATTACH */
    struct gpu_header header;
    uint32_t resource_id, padding;
};

struct __attribute__((packed)) gpu_ctx_create {
    struct gpu_header header;
    uint32_t name_length, context_init;
    char name[64];
};

struct __attribute__((packed)) gpu_transfer_3d {
    struct gpu_header header;
    struct gpu_box box;
    uint64_t offset;
    uint32_t resource_id, level, stride, layer_stride;
};

struct __attribute__((packed)) gpu_submit_3d {
    struct gpu_header header;
    uint32_t size, padding;
    uint8_t commands[];
};

struct __attribute__((packed)) gpu_capset_info_request {
    struct gpu_header header;
    uint32_t index, padding;
};

struct __attribute__((packed)) gpu_capset_info {
    struct gpu_header header;
    uint32_t id, max_version, max_size, padding;
};

struct __attribute__((packed)) gpu_capset_request {
    struct gpu_header header;
    uint32_t id, version;
};

/* ---- The control queue ---- */

#define QUEUE_SIZE 16
#define REQUEST_ORDER 7 /* 512 KiB, contiguous: the biggest request (a command buffer). */
#define REQUEST_BYTES (4096u << REQUEST_ORDER)
#define RESPONSE_ORDER 2 /* 16 KiB: the biggest answer (a capability set). */
#define RESPONSE_BYTES (4096u << RESPONSE_ORDER)
#define MAX_CAPSETS 8
#define MAX_RESOURCE_PAGES 65536 /* 256 MiB: mmap offsets keep the handle above this. */

struct __attribute__((packed)) avail_ring {
    uint16_t flags, index, ring[QUEUE_SIZE];
};

struct __attribute__((packed)) used_ring {
    uint16_t flags, index;
    struct {
        uint32_t id, length;
    } ring[QUEUE_SIZE];
};

struct capset {
    uint32_t id, max_version, max_size;
};

static struct {
    bool present;
    volatile uint16_t *notify;
    struct virtio_descriptor *descriptors;
    volatile struct avail_ring *avail;
    volatile struct used_ring *used;
    uint8_t *request, *response;
    uint16_t last_used;
    uint64_t fence;
    struct mutex lock; /* The queue (one request at a time), and the resource list. */
    struct device_waiter waiter;
    struct capset capsets[MAX_CAPSETS];
    int capset_count;
    uint32_t next_context, next_resource;
} gpu = {.lock = MUTEX_INIT};

static void gpu_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    device_wake(&gpu.waiter);
}

static bool answered(void *arg) {
    (void)arg;
    return gpu.used->index != gpu.last_used;
}

/* Sends what's in gpu.request (`length` bytes) and waits for the answer in
 * gpu.response; with `fence`, for the work to be done too. 0, or -VX_EIO if
 * the device didn't like it. Called with gpu.lock held. */
static int send(uint32_t length, bool fence, uint32_t ok) {
    struct gpu_header *header = (struct gpu_header *)gpu.request;
    if (fence) {
        header->flags |= FLAG_FENCE;
        header->fence_id = ++gpu.fence;
    }
    memset(gpu.response, 0, sizeof(struct gpu_header));
    gpu.descriptors[0] = (struct virtio_descriptor){
        .address = virt_to_phys(gpu.request), .length = length, .flags = VIRTIO_DESC_NEXT,
        .next = 1};
    gpu.descriptors[1] = (struct virtio_descriptor){
        .address = virt_to_phys(gpu.response), .length = RESPONSE_BYTES,
        .flags = VIRTIO_DESC_WRITE};
    gpu.avail->ring[gpu.avail->index % QUEUE_SIZE] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    gpu.avail->index++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *gpu.notify = 0; /* Queue 0. */
    device_wait(&gpu.waiter, answered, NULL);
    gpu.last_used = gpu.used->index;
    uint32_t type = ((struct gpu_header *)gpu.response)->type;
    if (type != ok) {
        kprintf("[virtio-gpu] request %x: answer %x\n", header->type, type);
        return -VX_EIO;
    }
    return 0;
}

/* A request of `size` bytes in gpu.request, zeroed, with its header set. */
static void *start(uint32_t type, uint32_t ctx_id, size_t size) {
    memset(gpu.request, 0, size);
    struct gpu_header *header = (struct gpu_header *)gpu.request;
    header->type = type;
    header->ctx_id = ctx_id;
    return gpu.request;
}

static int resource_command(uint32_t type, uint32_t ctx_id, uint32_t resource_id) {
    struct gpu_resource_command *c = start(type, ctx_id, sizeof(*c));
    c->resource_id = resource_id;
    return send(sizeof(*c), false, RESP_OK_NODATA);
}

/* ---- Contexts and resources ---- */

struct resource {
    uint32_t id; /* The host's, and the program's handle for it. */
    uint32_t size;
    uint64_t *pages;
    uint32_t page_count;
    struct context *context;
    struct resource *next;
};

struct context {
    uint32_t id;
};

static struct resource *resources;

static struct resource *find(struct context *context, uint32_t id) {
    for (struct resource *r = resources; r; r = r->next) {
        if (r->id == id && (!context || r->context == context)) {
            return r;
        }
    }
    return NULL;
}

static void free_pages(uint64_t *pages, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (pages[i]) {
            page_ref_put(pages[i]); /* (Mappings keep their own references.) */
        }
    }
    kfree(pages);
}

/* Takes the resource off the host and out of the list. Called with gpu.lock. */
static void destroy(struct resource *r) {
    resource_command(CMD_CTX_DETACH_RESOURCE, r->context->id, r->id);
    resource_command(CMD_RESOURCE_DETACH_BACKING, 0, r->id);
    resource_command(CMD_RESOURCE_UNREF, 0, r->id);
    for (struct resource **link = &resources; *link; link = &(*link)->next) {
        if (*link == r) {
            *link = r->next;
            break;
        }
    }
    free_pages(r->pages, r->page_count);
    kfree(r);
}

/* Gives the host the resource's pages: runs of contiguous ones as one entry. */
static int attach_backing(struct resource *r) {
    struct gpu_attach_backing *c = start(CMD_RESOURCE_ATTACH_BACKING, 0, sizeof(*c));
    uint32_t max_entries = (REQUEST_BYTES - sizeof(*c)) / sizeof(c->entries[0]), n = 0;
    c->resource_id = r->id;
    for (uint32_t i = 0; i < r->page_count; i++) {
        if (n && c->entries[n - 1].address + c->entries[n - 1].length == r->pages[i]) {
            c->entries[n - 1].length += 4096;
            continue;
        }
        if (n == max_entries) {
            return -VX_ENOMEM;
        }
        c->entries[n].address = r->pages[i];
        c->entries[n].length = 4096;
        c->entries[n].padding = 0;
        n++;
    }
    c->entry_count = n;
    return send(sizeof(*c) + n * sizeof(c->entries[0]), false, RESP_OK_NODATA);
}

/* ---- Linux's DRM requests (drm.h, virtgpu_drm.h) ---- */

#define IOC_NR(r) ((r)&0xff)
#define IOC_TYPE(r) (((r) >> 8) & 0xff)
#define IOC_SIZE(r) (((r) >> 16) & 0x3fff)

#define DRM_VERSION_NR 0x00
#define DRM_GEM_CLOSE_NR 0x09
#define VIRTGPU_MAP 0x41
#define VIRTGPU_EXECBUFFER 0x42
#define VIRTGPU_GETPARAM 0x43
#define VIRTGPU_RESOURCE_CREATE 0x44
#define VIRTGPU_RESOURCE_INFO 0x45
#define VIRTGPU_TRANSFER_FROM_HOST 0x46
#define VIRTGPU_TRANSFER_TO_HOST 0x47
#define VIRTGPU_WAIT 0x48
#define VIRTGPU_GET_CAPS 0x49

#define PARAM_3D_FEATURES 1
#define PARAM_CAPSET_QUERY_FIX 2
#define PARAM_SUPPORTED_CAPSET_IDS 7

struct drm_version {
    int32_t major, minor, patchlevel, pad;
    uint64_t name_length, name, date_length, date, desc_length, desc;
};

struct drm_execbuffer {
    uint32_t flags, size;
    uint64_t command, bo_handles;
    uint32_t bo_count;
    int32_t fence_fd;
};

struct drm_getparam {
    uint64_t param, value;
};

struct drm_resource_create {
    uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples,
        flags, bo_handle, res_handle, size, stride;
};

struct drm_resource_info {
    uint32_t bo_handle, res_handle, size, blob_mem;
};

struct drm_transfer {
    uint32_t bo_handle;
    struct gpu_box box;
    uint32_t level, offset, stride, layer_stride;
};

struct drm_get_caps {
    uint32_t id, version;
    uint64_t address;
    uint32_t size, pad;
};

/* One of drm_version's strings: as much as fits, and its whole length. */
static bool give_string(uint64_t *length, uint64_t address, const char *text) {
    size_t n = strlen(text);
    bool ok = !address || copy_to_user(address, text, n < *length ? n : *length);
    *length = n;
    return ok;
}

static int create(struct context *context, struct drm_resource_create *a) {
    uint32_t page_count = (a->size + 4095) / 4096;
    if (page_count == 0) {
        page_count = 1;
    }
    if (page_count > MAX_RESOURCE_PAGES) {
        return -VX_EINVAL;
    }
    struct resource *r = kzalloc(sizeof(*r));
    uint64_t *pages = kzalloc(page_count * sizeof(uint64_t));
    if (!r || !pages) {
        kfree(r);
        kfree(pages);
        return -VX_ENOMEM;
    }
    for (uint32_t i = 0; i < page_count; i++) {
        pages[i] = page_ref_new(); /* Zeroed. */
        if (!pages[i]) {
            free_pages(pages, page_count);
            kfree(r);
            return -VX_ENOMEM;
        }
    }
    r->pages = pages;
    r->page_count = page_count;
    r->size = a->size;
    r->context = context;
    r->id = ++gpu.next_resource;
    struct gpu_resource_create_3d *c = start(CMD_RESOURCE_CREATE_3D, 0, sizeof(*c));
    c->resource_id = r->id;
    c->target = a->target;
    c->format = a->format;
    c->bind = a->bind;
    c->width = a->width;
    c->height = a->height;
    c->depth = a->depth;
    c->array_size = a->array_size;
    c->last_level = a->last_level;
    c->nr_samples = a->nr_samples;
    c->flags = a->flags;
    int error = send(sizeof(*c), false, RESP_OK_NODATA);
    if (error) {
        free_pages(pages, page_count);
        kfree(r);
        return error;
    }
    r->next = resources;
    resources = r;
    error = attach_backing(r);
    if (!error) {
        error = resource_command(CMD_CTX_ATTACH_RESOURCE, context->id, r->id);
    }
    if (error) {
        destroy(r);
        return error;
    }
    a->bo_handle = a->res_handle = r->id;
    return 0;
}

static int transfer(struct context *context, uint32_t type, struct drm_transfer *a) {
    struct resource *r = find(context, a->bo_handle);
    if (!r) {
        return -VX_ENOENT;
    }
    struct gpu_transfer_3d *c = start(type, context->id, sizeof(*c));
    c->box = a->box;
    c->offset = a->offset;
    c->resource_id = r->id;
    c->level = a->level;
    c->stride = a->stride;
    c->layer_stride = a->layer_stride;
    return send(sizeof(*c), true, RESP_OK_NODATA);
}

static int submit(struct context *context, struct drm_execbuffer *a) {
    if (a->size > REQUEST_BYTES - sizeof(struct gpu_submit_3d) || a->size % 4) {
        return -VX_EINVAL;
    }
    struct gpu_submit_3d *c = start(CMD_SUBMIT_3D, context->id, sizeof(*c));
    c->size = a->size;
    if (!copy_from_user(c->commands, a->command, a->size)) {
        return -VX_EFAULT;
    }
    return send(sizeof(*c) + a->size, true, RESP_OK_NODATA);
}

static int get_caps(struct drm_get_caps *a) {
    const struct capset *set = NULL;
    for (int i = 0; i < gpu.capset_count; i++) {
        if (gpu.capsets[i].id == a->id) {
            set = &gpu.capsets[i];
        }
    }
    if (!set || a->version > set->max_version ||
        set->max_size > RESPONSE_BYTES - sizeof(struct gpu_header)) {
        return -VX_EINVAL;
    }
    struct gpu_capset_request *c = start(CMD_GET_CAPSET, 0, sizeof(*c));
    c->id = a->id;
    c->version = a->version;
    int error = send(sizeof(*c), false, RESP_OK_CAPSET);
    uint32_t size = a->size < set->max_size ? a->size : set->max_size;
    if (!error && !copy_to_user(a->address, gpu.response + sizeof(struct gpu_header), size)) {
        error = -VX_EFAULT;
    }
    return error;
}

static int gpu_control(struct file *file, uint32_t request, void *arg, size_t size) {
    struct context *context = file->private;
    uint32_t nr = IOC_NR(request);
    if (IOC_TYPE(request) != 'd') {
        return -VX_ENOTTY;
    }
    static const size_t sizes[] = {
        [DRM_VERSION_NR] = sizeof(struct drm_version),
        [DRM_GEM_CLOSE_NR] = 8,
        [VIRTGPU_MAP] = 16,
        [VIRTGPU_EXECBUFFER] = sizeof(struct drm_execbuffer),
        [VIRTGPU_GETPARAM] = sizeof(struct drm_getparam),
        [VIRTGPU_RESOURCE_CREATE] = sizeof(struct drm_resource_create),
        [VIRTGPU_RESOURCE_INFO] = sizeof(struct drm_resource_info),
        [VIRTGPU_TRANSFER_FROM_HOST] = sizeof(struct drm_transfer),
        [VIRTGPU_TRANSFER_TO_HOST] = sizeof(struct drm_transfer),
        [VIRTGPU_WAIT] = 8,
        [VIRTGPU_GET_CAPS] = sizeof(struct drm_get_caps),
    };
    if (nr >= sizeof(sizes) / sizeof(sizes[0]) || !sizes[nr]) {
        return -VX_EINVAL; /* (PRIME, blobs, context parameters, fence files: not here.) */
    }
    if (size < sizes[nr]) {
        return -VX_EINVAL;
    }
    int result = 0;
    mutex_lock(&gpu.lock);
    switch (nr) {
    case DRM_VERSION_NR: {
        struct drm_version *v = arg;
        v->major = 0; /* (Minor 0: no fence files, so Mesa waits on resources.) */
        v->minor = 0;
        v->patchlevel = 0;
        if (!give_string(&v->name_length, v->name, "virtio_gpu") ||
            !give_string(&v->date_length, v->date, "0") ||
            !give_string(&v->desc_length, v->desc, "virtio GPU (Vexa)")) {
            result = -VX_EFAULT;
        }
        break;
    }
    case DRM_GEM_CLOSE_NR: {
        struct resource *r = find(context, *(uint32_t *)arg);
        if (r) {
            destroy(r);
        } else {
            result = -VX_EINVAL;
        }
        break;
    }
    case VIRTGPU_MAP: { /* The offset to mmap the device at: see share_page. */
        uint64_t *offset = arg;
        uint32_t handle = ((uint32_t *)arg)[2];
        if (find(context, handle)) {
            *offset = (uint64_t)handle << 28;
        } else {
            result = -VX_ENOENT;
        }
        break;
    }
    case VIRTGPU_EXECBUFFER:
        result = submit(context, arg);
        break;
    case VIRTGPU_GETPARAM: {
        struct drm_getparam *p = arg;
        uint64_t value = 0;
        if (p->param == PARAM_3D_FEATURES || p->param == PARAM_CAPSET_QUERY_FIX) {
            value = 1;
        } else if (p->param == PARAM_SUPPORTED_CAPSET_IDS) {
            for (int i = 0; i < gpu.capset_count; i++) {
                value |= 1ull << gpu.capsets[i].id;
            }
        } else {
            result = -VX_EINVAL;
            break;
        }
        if (!copy_to_user(p->value, &value, sizeof(value))) {
            result = -VX_EFAULT;
        }
        break;
    }
    case VIRTGPU_RESOURCE_CREATE:
        result = create(context, arg);
        break;
    case VIRTGPU_RESOURCE_INFO: {
        struct drm_resource_info *info = arg;
        struct resource *r = find(context, info->bo_handle);
        if (r) {
            info->res_handle = r->id;
            info->size = r->size;
            info->blob_mem = 0;
        } else {
            result = -VX_ENOENT;
        }
        break;
    }
    case VIRTGPU_TRANSFER_FROM_HOST:
        result = transfer(context, CMD_TRANSFER_FROM_HOST_3D, arg);
        break;
    case VIRTGPU_TRANSFER_TO_HOST:
        result = transfer(context, CMD_TRANSFER_TO_HOST_3D, arg);
        break;
    case VIRTGPU_WAIT: /* Every request has finished by the time it returns. */
        result = find(context, *(uint32_t *)arg) ? 0 : -VX_ENOENT;
        break;
    case VIRTGPU_GET_CAPS:
        result = get_caps(arg);
        break;
    }
    mutex_unlock(&gpu.lock);
    return result;
}

static int gpu_open(struct file *file) {
    struct context *context = kzalloc(sizeof(*context));
    if (!context) {
        return -VX_ENOMEM;
    }
    mutex_lock(&gpu.lock);
    context->id = ++gpu.next_context;
    struct gpu_ctx_create *c = start(CMD_CTX_CREATE, context->id, sizeof(*c));
    memcpy(c->name, "vexa", 4);
    c->name_length = 4;
    int error = send(sizeof(*c), false, RESP_OK_NODATA);
    mutex_unlock(&gpu.lock);
    if (error) {
        kfree(context);
        return error;
    }
    file->private = context;
    return 0;
}

static void gpu_close(struct file *file) {
    struct context *context = file->private;
    mutex_lock(&gpu.lock);
    for (struct resource *r = resources, *next; r; r = next) {
        next = r->next;
        if (r->context == context) {
            destroy(r);
        }
    }
    start(CMD_CTX_DESTROY, context->id, sizeof(struct gpu_header));
    send(sizeof(struct gpu_header), false, RESP_OK_NODATA);
    mutex_unlock(&gpu.lock);
    kfree(context);
}

/* mmap at MAP's offset: the handle (above page 65536), then the page. */
static uint64_t gpu_share_page(struct vnode *vnode, uint64_t index) {
    (void)vnode;
    mutex_lock(&gpu.lock);
    struct resource *r = find(NULL, (uint32_t)(index >> 16));
    uint64_t page = index & 0xffff, phys = 0;
    if (r && page < r->page_count) {
        phys = r->pages[page];
        page_ref_get(phys);
    }
    mutex_unlock(&gpu.lock);
    return phys;
}

static const struct vnode_ops gpu_ops = {
    .open = gpu_open,
    .close = gpu_close,
    .control = gpu_control,
    .share_page = gpu_share_page,
};

bool virtio_gpu_file(struct file *file) {
    return file->vnode->ops == &gpu_ops;
}

int64_t virtio_gpu_ioctl(struct file *file, uint32_t request, uint64_t arg) {
    uint8_t buffer[128];
    size_t size = IOC_SIZE(request);
    if (size > sizeof(buffer)) {
        return -VX_EINVAL;
    }
    if (size && !copy_from_user(buffer, arg, size)) {
        return -VX_EFAULT;
    }
    int result = gpu_control(file, request, buffer, size);
    if (result >= 0 && size && !copy_to_user(arg, buffer, size)) {
        return -VX_EFAULT;
    }
    return result;
}

/* ---- Finding it ---- */

static void probe(struct pci_device *pci) {
    struct virtio_device device;
    uint32_t features;
    if (!virtio_find(pci, &device) ||
        !virtio_negotiate(&device, 1U << GPU_FEATURE_VIRGL, &features)) {
        return;
    }
    if (!(features & (1U << GPU_FEATURE_VIRGL))) {
        kprintf("[virtio-gpu] no 3D (virgl) on this one: not used\n");
        device.common->device_status = 0;
        return;
    }
    volatile struct virtio_common_cfg *common = device.common;
    volatile uint32_t *config = device.device_config; /* events_read, events_clear, scanouts, capsets */
    gpu.descriptors = virtio_dma_page();
    gpu.avail = virtio_dma_page();
    gpu.used = virtio_dma_page();
    uint64_t request = pmm_alloc(REQUEST_ORDER), response = pmm_alloc(RESPONSE_ORDER);
    if (!gpu.descriptors || !gpu.avail || !gpu.used || !request || !response) {
        kprintf("[virtio-gpu] out of memory\n");
        common->device_status = 0;
        return;
    }
    gpu.request = phys_to_virt(request);
    gpu.response = phys_to_virt(response);
    common->queue_select = 0;
    uint16_t size = common->queue_size < QUEUE_SIZE ? common->queue_size : QUEUE_SIZE;
    common->queue_size = size;
    common->queue_desc = virt_to_phys(gpu.descriptors);
    common->queue_driver = virt_to_phys((void *)gpu.avail);
    common->queue_device = virt_to_phys((void *)gpu.used);
    gpu.notify = virtio_queue_notify(&device);
    if (pci_enable_msi(pci, gpu_interrupt)) {
        common->msix_config = VIRTIO_MSIX_NO_VECTOR;
        common->queue_msix_vector = 0;
        gpu.waiter.has_interrupt = common->queue_msix_vector == 0;
    }
    common->queue_enable = 1;
    common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;

    /* Its capability sets (virgl's, which Mesa reads). */
    uint32_t capsets = config[3];
    for (uint32_t i = 0; i < capsets && gpu.capset_count < MAX_CAPSETS; i++) {
        struct gpu_capset_info_request *c = start(CMD_GET_CAPSET_INFO, 0, sizeof(*c));
        c->index = i;
        if (send(sizeof(*c), false, RESP_OK_CAPSET_INFO) == 0) {
            struct gpu_capset_info *info = (struct gpu_capset_info *)gpu.response;
            gpu.capsets[gpu.capset_count++] =
                (struct capset){info->id, info->max_version, info->max_size};
        }
    }
    gpu.present = true;
    pci_claim(pci, "virtio-gpu", "Virtio GPU (3D)");
    devfs_add("dri/renderD128", &gpu_ops, NULL);
    kprintf("[virtio-gpu] 3D (virgl): /dev/dri/renderD128, %d capability set(s), %s\n",
            gpu.capset_count, gpu.waiter.has_interrupt ? "MSI-X interrupts" : "polling");
}

void virtio_gpu_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->vendor_id == VIRTIO_VENDOR && pci->device_id == VIRTIO_GPU_MODERN &&
            !gpu.present) {
            probe(pci);
        }
    }
}
