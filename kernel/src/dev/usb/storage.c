/*
 * USB mass storage: USB sticks, card readers and disks, as block devices
 * (usb0, usb1...; partitions usb0p1...), mounted at /mnt like other disks.
 *
 * Bulk-only transport: each SCSI command goes out in a 31-byte command
 * block wrapper on the bulk OUT endpoint, its data moves on the bulk
 * endpoints, and a 13-byte status wrapper comes back on bulk IN. Commands
 * used: INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY (10, 16),
 * READ and WRITE (10, 16).
 */
#include <vexa/block.h>
#include <vexa/device.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/sched.h>
#include <vexa/storage.h>
#include <vexa/string.h>
#include <vexa/usb.h>

#define CBW_SIGNATURE 0x43425355 /* "USBC" */
#define CSW_SIGNATURE 0x53425355 /* "USBS" */
#define REQ_RESET 0xff
#define REQ_GET_MAX_LUN 0xfe
#define TRANSFER_MAX 65536

struct __attribute__((packed)) cbw {
    uint32_t signature, tag, length;
    uint8_t flags, lun, command_length;
    uint8_t command[16];
};

struct __attribute__((packed)) csw {
    uint32_t signature, tag, residue;
    uint8_t status;
};

struct storage {
    struct usb_device *usb;
    struct usb_interface *interface;
    uint8_t in, out; /* Bulk endpoints. */
    uint32_t tag;
    struct mutex lock;
    uint8_t *page;   /* Wrappers and small replies (in the direct map, one page). */
    struct block_device block;
    bool big;        /* More than 2^32 sectors: the 16-byte commands. */
    char vendor[9], product[17];
};

static void reset_recovery(struct storage *s) {
    usb_control(s->usb, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, REQ_RESET, 0,
                s->interface->number, NULL, 0);
    usb_clear_halt(s->usb, s->in);
    usb_clear_halt(s->usb, s->out);
}

/* One SCSI command: data in (`in`) or out, into or from `data` (direct map).
 * Returns the bytes moved, or a negative error (-VX_EIO: the command failed). */
static int command(struct storage *s, const uint8_t *cdb, int cdb_length, void *data,
                   uint32_t length, bool in) {
    struct cbw *cbw = (struct cbw *)s->page;
    struct csw *csw = (struct csw *)(s->page + 64);
    memset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIGNATURE;
    cbw->tag = ++s->tag;
    cbw->length = length;
    cbw->flags = in ? 0x80 : 0;
    cbw->command_length = (uint8_t)cdb_length;
    memcpy(cbw->command, cdb, (size_t)cdb_length);
    int n = usb_bulk(s->usb, s->out, cbw, sizeof(*cbw), 5000);
    if (n != (int)sizeof(*cbw)) {
        kprintf("[usb-storage] %s: command %02x: sending it failed (%d)\n", s->block.name, cdb[0], n);
        reset_recovery(s);
        return n < 0 ? n : -VX_EIO;
    }
    int moved = 0;
    if (length) {
        moved = usb_bulk(s->usb, in ? s->in : s->out, data, length, 20000);
        if (moved == -VX_EPIPE) {
            usb_clear_halt(s->usb, in ? s->in : s->out); /* Then the status still comes. */
            moved = 0;
        } else if (moved < 0) {
            kprintf("[usb-storage] %s: command %02x: its data failed (%d)\n", s->block.name, cdb[0],
                    moved);
            reset_recovery(s);
            return moved;
        }
    }
    n = usb_bulk(s->usb, s->in, csw, sizeof(*csw), 5000);
    if (n == -VX_EPIPE) {
        usb_clear_halt(s->usb, s->in);
        n = usb_bulk(s->usb, s->in, csw, sizeof(*csw), 5000);
    }
    if (n != (int)sizeof(*csw) || csw->signature != CSW_SIGNATURE || csw->tag != cbw->tag ||
        csw->status == 2) {
        kprintf("[usb-storage] %s: command %02x: no good status (%d)\n", s->block.name, cdb[0], n);
        reset_recovery(s);
        return n < 0 ? n : -VX_EIO;
    }
    return csw->status == 0 ? moved : -VX_EIO;
}

static void request_sense(struct storage *s) {
    uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
    command(s, cdb, 6, s->page + 128, 18, true);
}

static bool unit_ready(struct storage *s) {
    for (int tries = 0; tries < 25; tries++) {
        uint8_t cdb[6] = {0x00};
        if (command(s, cdb, 6, NULL, 0, false) == 0) {
            return true;
        }
        request_sense(s); /* (Clears "medium changed" and such.) */
        thread_sleep_ms(200);
    }
    return false;
}

static bool read_capacity(struct storage *s) {
    uint8_t cdb10[10] = {0x25};
    uint8_t *reply = s->page + 128;
    if (command(s, cdb10, 10, reply, 8, true) < 8) {
        return false;
    }
    uint32_t last = (uint32_t)reply[0] << 24 | reply[1] << 16 | reply[2] << 8 | reply[3];
    uint32_t size = (uint32_t)reply[4] << 24 | reply[5] << 16 | reply[6] << 8 | reply[7];
    uint64_t count = (uint64_t)last + 1;
    if (last == 0xffffffff) { /* Bigger: READ CAPACITY (16). */
        uint8_t cdb16[16] = {0x9e, 0x10};
        cdb16[13] = 32;
        if (command(s, cdb16, 16, reply, 32, true) < 12) {
            return false;
        }
        count = 0;
        for (int i = 0; i < 8; i++) {
            count = count << 8 | reply[i];
        }
        count++;
        size = (uint32_t)reply[8] << 24 | reply[9] << 16 | reply[10] << 8 | reply[11];
        s->big = true;
    }
    if (size != 512 && size != 1024 && size != 2048 && size != 4096) {
        return false;
    }
    s->block.sector_count = count;
    s->block.sector_size = size;
    return true;
}

static int io(struct block_device *block, uint64_t sector, uint32_t count, void *buffer,
              bool write) {
    struct storage *s = block->driver_data;
    if (block->gone) {
        return -VX_EIO;
    }
    mutex_lock(&s->lock);
    int error = 0;
    uint8_t *p = buffer;
    uint32_t per_transfer = TRANSFER_MAX / block->sector_size;
    while (count && !error) {
        uint32_t n = count < per_transfer ? count : per_transfer;
        uint8_t cdb[16] = {0};
        int cdb_length;
        if (s->big) {
            cdb[0] = write ? 0x8a : 0x88; /* WRITE (16), READ (16) */
            for (int i = 0; i < 8; i++) {
                cdb[2 + i] = (uint8_t)(sector >> (56 - 8 * i));
            }
            cdb[10] = (uint8_t)(n >> 24);
            cdb[11] = (uint8_t)(n >> 16);
            cdb[12] = (uint8_t)(n >> 8);
            cdb[13] = (uint8_t)n;
            cdb_length = 16;
        } else {
            cdb[0] = write ? 0x2a : 0x28; /* WRITE (10), READ (10) */
            cdb[2] = (uint8_t)(sector >> 24);
            cdb[3] = (uint8_t)(sector >> 16);
            cdb[4] = (uint8_t)(sector >> 8);
            cdb[5] = (uint8_t)sector;
            cdb[7] = (uint8_t)(n >> 8);
            cdb[8] = (uint8_t)n;
            cdb_length = 10;
        }
        uint32_t bytes = n * block->sector_size;
        int moved = command(s, cdb, cdb_length, p, bytes, !write);
        if (moved != (int)bytes) {
            request_sense(s);
            error = -VX_EIO;
        }
        p += bytes;
        sector += n;
        count -= n;
    }
    mutex_unlock(&s->lock);
    return error;
}

static int storage_read(struct block_device *block, uint64_t sector, uint32_t count, void *buffer) {
    return io(block, sector, count, buffer, false);
}

static int storage_write(struct block_device *block, uint64_t sector, uint32_t count,
                         const void *buffer) {
    return io(block, sector, count, (void *)buffer, true);
}

/* "usb0", or the first number free (an unplugged stick's comes back). */
static void choose_name(char *name, size_t size) {
    for (int i = 0; i < 100; i++) {
        ksnprintf(name, size, "usb%d", i);
        if (!block_find(name)) {
            return;
        }
    }
}

static void trim(char *text) {
    size_t n = strlen(text);
    while (n && text[n - 1] == ' ') {
        text[--n] = '\0';
    }
}

static bool usb_storage_probe(struct usb_interface *interface) {
    if (interface->interface_class != USB_CLASS_STORAGE || interface->subclass != 6 ||
        interface->protocol != 0x50) {
        return false; /* Only SCSI over bulk-only transport (what sticks are). */
    }
    struct storage *s = kzalloc(sizeof(*s));
    if (!s) {
        return false;
    }
    s->usb = interface->device;
    s->interface = interface;
    s->lock = (struct mutex)MUTEX_INIT;
    for (int i = 0; i < interface->endpoint_count; i++) {
        const struct usb_endpoint *e = &interface->endpoints[i];
        if (e->type == USB_ENDPOINT_BULK) {
            if (e->address & USB_DIR_IN) {
                s->in = e->address;
            } else {
                s->out = e->address;
            }
        }
    }
    uint64_t phys = pmm_alloc(0);
    s->page = phys ? phys_to_virt(phys) : NULL;
    if (!s->in || !s->out || !s->page) {
        if (s->page) {
            pmm_free(phys, 0);
        }
        kfree(s);
        return false;
    }
    memset(s->page, 0, PAGE_SIZE);

    /* What it is, and whether there's a medium in it. */
    uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t *reply = s->page + 128;
    if (command(s, inquiry, 6, reply, 36, true) >= 36) {
        memcpy(s->vendor, reply + 8, 8);
        memcpy(s->product, reply + 16, 16);
        trim(s->vendor);
        trim(s->product);
    }
    char name[64];
    usb_device_name(s->usb, name, sizeof(name));
    interface->driver_data = s;
    if (!unit_ready(s) || !read_capacity(s)) {
        kprintf("[usb-storage] %s: no medium\n", name);
        return true; /* (Ours, but with nothing to read: a card reader without a card.) */
    }
    choose_name(s->block.name, sizeof(s->block.name));
    s->block.read = storage_read;
    s->block.write = storage_write;
    s->block.driver_data = s;
    s->block.description = "USB drive";
    s->block.controller = s->usb->node;
    kprintf("[usb-storage] %s: %s %s, %lu MiB\n", s->block.name, s->vendor, s->product,
            (unsigned long)(block_size_bytes(&s->block) >> 20));
    block_register(&s->block);
    if (s->block.node && (s->vendor[0] || s->product[0])) {
        char label[40];
        ksnprintf(label, sizeof(label), "%s %s", s->vendor, s->product);
        device_set_name(s->block.node, label);
    }
    device_set_driver(s->block.node, "usb-storage");
    storage_mount_disk(&s->block);
    return true;
}

static void usb_storage_disconnect(struct usb_interface *interface) {
    struct storage *s = interface->driver_data;
    if (!s) {
        return;
    }
    if (s->block.name[0]) {
        block_unregister(&s->block);
    }
    /* (The structure stays, as the block device's: files may still be open on it.) */
    interface->driver_data = NULL;
}

const struct usb_driver usb_storage_driver = {
    .name = "usb-storage",
    .probe = usb_storage_probe,
    .disconnect = usb_storage_disconnect,
};
