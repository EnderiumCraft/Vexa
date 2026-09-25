#include <vexa/abi.h>
#include <vexa/console.h>
#include <vexa/fb.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * /dev/display0: the boot framebuffer, for programs. A program acquires it
 * (the text console stops drawing), maps the frame buffer with vx_map_file,
 * and draws; when its handle closes, the console comes back.
 */

static struct vx_display_info info;
static uint64_t frame_phys;
static struct file *owner; /* The file that acquired the display. */
static struct spinlock lock = SPINLOCK_INIT;

static void display_close(struct file *file) {
    uint64_t flags = spin_lock_irqsave(&lock);
    bool was_owner = owner == file;
    if (was_owner) {
        owner = NULL;
    }
    spin_unlock_irqrestore(&lock, flags);
    if (was_owner) {
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
    frame_phys = virt_to_phys(fb->address);
    for (uint64_t offset = 0; offset < info.size; offset += PAGE_SIZE) {
        page_ref_pin(frame_phys + offset);
    }
    devfs_add("display0", &display_ops, NULL);
    kprintf("[display] display0: %ux%u, %u bits per pixel\n", info.width, info.height,
            info.bits_per_pixel);
}
