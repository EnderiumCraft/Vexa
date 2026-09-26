#include <vexa/abi.h>
#include <vexa/console.h>
#include <vexa/fb.h>
#include <vexa/fs.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/pci.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * /dev/display0: the boot framebuffer, for programs. A program acquires it
 * (the text console stops drawing), maps the frame buffer with vx_map_file,
 * and draws; when its handle closes, the console comes back.
 *
 * On QEMU's and Bochs's standard VGA (PCI 1234:1111), the holder can also
 * change the resolution, through the card's "DISPI" registers; the first
 * mode comes back when it lets go, for the console.
 */

static struct vx_display_info info, boot_info;
static uint64_t frame_phys;
static struct file *owner; /* The file that acquired the display. */
static struct spinlock lock = SPINLOCK_INIT;

/* ---- Mode setting (Bochs DISPI) ---- */

#define DISPI_INDEX 0x1ce
#define DISPI_DATA 0x1cf
enum {
    DISPI_ID, DISPI_XRES, DISPI_YRES, DISPI_BPP, DISPI_ENABLE, DISPI_BANK, DISPI_VIRT_WIDTH,
    DISPI_VIRT_HEIGHT, DISPI_X_OFFSET, DISPI_Y_OFFSET, DISPI_VIDEO_MEMORY_64K,
};
#define DISPI_ENABLED 0x01
#define DISPI_LFB 0x40

static bool dispi_checked, dispi;
static uint64_t vram_size;

static const struct vx_display_mode standard_modes[] = {
    {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},  {1280, 800},  {1280, 1024},
    {1366, 768},  {1440, 900},  {1600, 900},  {1600, 1200}, {1680, 1050}, {1920, 1080},
    {1920, 1200}, {2560, 1440}, {2560, 1600},
};

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_INDEX, index);
    return inw(DISPI_DATA);
}

static void dispi_write(uint16_t index, uint16_t value) {
    outw(DISPI_INDEX, index);
    outw(DISPI_DATA, value);
}

/* Is the boot frame buffer a DISPI card's? (Looked at the first time it's
 * needed: PCI is scanned after the display starts.) */
static void check_dispi(void) {
    if (dispi_checked) {
        return;
    }
    dispi_checked = true;
    for (struct pci_device *d = pci_first(); d; d = d->next) {
        if (d->vendor_id != 0x1234 || d->device_id != 0x1111 || d->bar_is_io[0] ||
            frame_phys < d->bar[0] || frame_phys >= d->bar[0] + d->bar_size[0]) {
            continue;
        }
        uint16_t id = dispi_read(DISPI_ID);
        if (id < 0xb0c0 || id > 0xb0cf) {
            continue;
        }
        vram_size = (uint64_t)dispi_read(DISPI_VIDEO_MEMORY_64K) * 64 * 1024;
        if (!vram_size) {
            vram_size = d->bar_size[0];
        }
        dispi = true;
        kprintf("[display] Bochs/QEMU standard VGA: %lu MiB, modes can be set\n",
                (unsigned long)(vram_size >> 20));
        return;
    }
}

static bool mode_fits(unsigned width, unsigned height) {
    return (uint64_t)width * height * 4 <= vram_size && width <= 4096 && height <= 4096;
}

static void set_info(unsigned width, unsigned height) {
    info.width = width;
    info.height = height;
    info.pitch = width * 4;
    info.size = (unsigned)(((uint64_t)info.pitch * height + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1));
}

static void dispi_set(unsigned width, unsigned height) {
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (uint16_t)width);
    dispi_write(DISPI_YRES, (uint16_t)height);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, (uint16_t)width);
    dispi_write(DISPI_VIRT_HEIGHT, (uint16_t)height);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
    set_info(width, height);
}

static void list_modes(struct vx_display_modes *out) {
    memset(out, 0, sizeof(*out));
    check_dispi();
    bool boot_listed = false;
    for (size_t i = 0; dispi && i < sizeof(standard_modes) / sizeof(standard_modes[0]); i++) {
        const struct vx_display_mode *m = &standard_modes[i];
        if (!mode_fits(m->width, m->height) || out->count == VX_DISPLAY_MAX_MODES) {
            continue;
        }
        /* The boot mode goes in its place by size. */
        if (!boot_listed && (uint64_t)m->width * m->height >=
                                (uint64_t)boot_info.width * boot_info.height) {
            boot_listed = true;
            if (m->width != boot_info.width || m->height != boot_info.height) {
                out->modes[out->count++] = (struct vx_display_mode){boot_info.width, boot_info.height};
            }
        }
        if (out->count < VX_DISPLAY_MAX_MODES) {
            out->modes[out->count++] = *m;
        }
    }
    if (!boot_listed && out->count < VX_DISPLAY_MAX_MODES) {
        out->modes[out->count++] = (struct vx_display_mode){boot_info.width, boot_info.height};
    }
    for (unsigned i = 0; i < out->count; i++) {
        if (out->modes[i].width == info.width && out->modes[i].height == info.height) {
            out->current = i;
        }
    }
}

static void display_close(struct file *file) {
    uint64_t flags = spin_lock_irqsave(&lock);
    bool was_owner = owner == file;
    if (was_owner) {
        owner = NULL;
    }
    spin_unlock_irqrestore(&lock, flags);
    if (was_owner) {
        if (dispi && (info.width != boot_info.width || info.height != boot_info.height)) {
            dispi_set(boot_info.width, boot_info.height); /* The console's mode. */
        }
        fb_set_hidden(false);
        console_redraw();
    }
}

static int display_control(struct file *file, uint32_t request, void *arg, size_t size) {
    switch (request) {
    case VX_DISPLAY_INFO:
        if (size < sizeof(info)) {
            return -VX_EINVAL;
        }
        memcpy(arg, &info, sizeof(info));
        return 0;
    case VX_DISPLAY_ACQUIRE: {
        uint64_t flags = spin_lock_irqsave(&lock);
        int error = owner && owner != file ? -VX_EBUSY : 0;
        if (!error) {
            owner = file;
        }
        spin_unlock_irqrestore(&lock, flags);
        if (!error) {
            fb_set_hidden(true);
        }
        return error;
    }
    case VX_DISPLAY_MODES:
        if (size < sizeof(struct vx_display_modes)) {
            return -VX_EINVAL;
        }
        list_modes(arg);
        return 0;
    case VX_DISPLAY_SET_MODE: {
        if (size < sizeof(struct vx_display_mode)) {
            return -VX_EINVAL;
        }
        const struct vx_display_mode *mode = arg;
        check_dispi();
        if (owner != file) {
            return -VX_EACCES;
        }
        if (mode->width == info.width && mode->height == info.height) {
            return 0;
        }
        if (!dispi || !mode_fits(mode->width, mode->height) || mode->width < 640 ||
            mode->height < 480) {
            return -VX_EINVAL;
        }
        dispi_set(mode->width, mode->height);
        kprintf("[display] now %ux%u\n", info.width, info.height);
        return 0;
    }
    default:
        return -VX_ENOTTY;
    }
}

static uint64_t display_page(struct vnode *vnode, uint64_t index) {
    (void)vnode;
    if (index * PAGE_SIZE >= info.size) {
        return 0;
    }
    uint64_t phys = frame_phys + index * PAGE_SIZE;
    page_ref_get(phys); /* The mapping's reference (pinned: never freed). */
    return phys;
}

static const struct vnode_ops display_ops = {
    .close = display_close,
    .control = display_control,
    .share_page = display_page,
};

void display_init(void) {
    const struct limine_framebuffer *fb = fb_limine();
    if (!fb) {
        return;
    }
    info.width = (unsigned)fb->width;
    info.height = (unsigned)fb->height;
    info.pitch = (unsigned)fb->pitch;
    info.bits_per_pixel = fb->bpp;
    info.red_shift = fb->red_mask_shift;
    info.green_shift = fb->green_mask_shift;
    info.blue_shift = fb->blue_mask_shift;
    info.size = (unsigned)((fb->pitch * fb->height + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    boot_info = info;
    frame_phys = virt_to_phys(fb->address);
    for (uint64_t offset = 0; offset < info.size; offset += PAGE_SIZE) {
        page_ref_pin(frame_phys + offset);
    }
    devfs_add("display0", &display_ops, NULL);
    kprintf("[display] display0: %ux%u, %u bits per pixel\n", info.width, info.height,
            info.bits_per_pixel);
}
