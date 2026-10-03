/*
 * IDE (parallel ATA, or SATA in its "IDE" mode): the disk controllers of
 * older PCs, the ones VirtualBox gives a new virtual machine for its CD
 * drive, and QEMU's "pc" machine (PIIX). Two channels, each with up to two
 * drives (master and slave): disks (hda, hdb...) and CD/DVD drives (cd0...,
 * through ATAPI: SCSI commands in a PACKET command).
 *
 * Data moves by PIO (16 bits at a time through the data register), and the
 * driver waits by checking the status register, without interrupts: simple,
 * and fast enough for a CD or a small disk. The controller's own DMA engine
 * (bus mastering) is left alone.
 */
#include <vexa/arch.h>
#include <vexa/block.h>
#include <vexa/device.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/sched.h>
#include <vexa/string.h>

/* Registers, from a channel's command block. */
#define REG_DATA 0
#define REG_ERROR 1
#define REG_FEATURES 1
#define REG_COUNT 2
#define REG_LBA0 3
#define REG_LBA1 4 /* (ATAPI: the byte count, low.) */
#define REG_LBA2 5 /* (ATAPI: the byte count, high.) */
#define REG_DEVICE 6
#define REG_STATUS 7
#define REG_COMMAND 7
/* From its control block. */
#define REG_ALT_STATUS 0
#define REG_CONTROL 0

#define STATUS_ERROR 0x01
#define STATUS_DRQ 0x08 /* Data to move. */
#define STATUS_FAULT 0x20
#define STATUS_READY 0x40
#define STATUS_BUSY 0x80
#define CONTROL_NO_INTERRUPTS 0x02
#define CONTROL_RESET 0x04

#define CMD_READ 0x20
#define CMD_READ_EXT 0x24
#define CMD_WRITE 0x30
#define CMD_WRITE_EXT 0x34
#define CMD_FLUSH 0xe7
#define CMD_FLUSH_EXT 0xea
#define CMD_PACKET 0xa0
#define CMD_IDENTIFY_PACKET 0xa1
#define CMD_IDENTIFY 0xec

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_READ_CAPACITY 0x25
#define SCSI_READ_10 0x28

#define CD_SECTOR 2048
#define MAX_DRIVES 8

struct channel {
    uint16_t command, control;
    struct mutex lock; /* (Its two drives share it.) */
};

struct drive {
    struct block_device block; /* (First: the block layer's pointer is the drive's.) */
    struct channel *channel;
    bool slave, atapi, lba48;
};

static struct drive *drives[MAX_DRIVES];
static int drive_count, disk_count;

static void rep_insw(uint16_t port, void *buffer, uint32_t words) {
    __asm__ volatile("rep insw" : "+D"(buffer), "+c"(words) : "d"(port) : "memory");
}

static void rep_outsw(uint16_t port, const void *buffer, uint32_t words) {
    __asm__ volatile("rep outsw" : "+S"(buffer), "+c"(words) : "d"(port) : "memory");
}

static uint8_t status_of(struct channel *c) {
    return inb((uint16_t)(c->command + REG_STATUS));
}

/* About 400 ns: what a drive needs after being selected or given a command. */
static void settle(struct channel *c) {
    for (int i = 0; i < 4; i++) {
        inb((uint16_t)(c->control + REG_ALT_STATUS));
    }
}

/* Waits for BUSY to clear (and, with `drq`, for DRQ or an error). Returns
 * the status, or 0xff on a timeout. */
static uint8_t wait_ready(struct channel *c, bool drq, uint32_t timeout_ms) {
    uint64_t deadline = timer_ms() + timeout_ms;
    for (int spins = 0;; spins++) {
        uint8_t status = status_of(c);
        if (!(status & STATUS_BUSY) &&
            (!drq || (status & (STATUS_DRQ | STATUS_ERROR | STATUS_FAULT)))) {
            return status;
        }
        if (timer_ms() > deadline) {
            return 0xff;
        }
        if (spins > 1000) {
            thread_yield();
        }
    }
}

static void select_drive(struct channel *c, bool slave, uint8_t lba_bits) {
    outb((uint16_t)(c->command + REG_DEVICE), (uint8_t)(0xe0 | (slave ? 0x10 : 0) | lba_bits));
    settle(c);
}

/* ---- Disks ---- */

static void set_lba(struct drive *d, uint64_t lba, uint32_t count) {
    struct channel *c = d->channel;
    if (d->lba48) {
        select_drive(c, d->slave, 0);
        outb((uint16_t)(c->command + REG_COUNT), (uint8_t)(count >> 8));
        outb((uint16_t)(c->command + REG_LBA0), (uint8_t)(lba >> 24));
        outb((uint16_t)(c->command + REG_LBA1), (uint8_t)(lba >> 32));
        outb((uint16_t)(c->command + REG_LBA2), (uint8_t)(lba >> 40));
    } else {
        select_drive(c, d->slave, (uint8_t)((lba >> 24) & 0x0f));
    }
    outb((uint16_t)(c->command + REG_COUNT), (uint8_t)count);
    outb((uint16_t)(c->command + REG_LBA0), (uint8_t)lba);
    outb((uint16_t)(c->command + REG_LBA1), (uint8_t)(lba >> 8));
    outb((uint16_t)(c->command + REG_LBA2), (uint8_t)(lba >> 16));
}

static int disk_transfer(struct block_device *block, uint64_t sector, uint32_t count,
                         void *buffer, bool write) {
    struct drive *d = (struct drive *)block;
    struct channel *c = d->channel;
    uint32_t most = d->lba48 ? 65536 : 256;
    uint8_t *p = buffer;
    int result = 0;
    mutex_lock(&c->lock);
    while (count && result == 0) {
        uint32_t n = count < most ? count : most;
        set_lba(d, sector, n == most ? 0 : n); /* (0: the most there can be.) */
        outb((uint16_t)(c->command + REG_COMMAND),
             write ? (d->lba48 ? CMD_WRITE_EXT : CMD_WRITE) : (d->lba48 ? CMD_READ_EXT : CMD_READ));
        settle(c);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t status = wait_ready(c, true, 5000);
            if (status == 0xff || (status & (STATUS_ERROR | STATUS_FAULT)) ||
                !(status & STATUS_DRQ)) {
                result = -VX_EIO;
                break;
            }
            if (write) {
                rep_outsw((uint16_t)(c->command + REG_DATA), p, block->sector_size / 2);
            } else {
                rep_insw((uint16_t)(c->command + REG_DATA), p, block->sector_size / 2);
            }
            p += block->sector_size;
        }
        if (result == 0 && write) {
            /* Written for real before saying so (Vexa's writes go straight through). */
            uint8_t status = wait_ready(c, false, 5000);
            outb((uint16_t)(c->command + REG_COMMAND), d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
            settle(c);
            status = wait_ready(c, false, 30000);
            if (status == 0xff || (status & (STATUS_ERROR | STATUS_FAULT))) {
                result = -VX_EIO;
            }
        }
        sector += n;
        count -= n;
    }
    mutex_unlock(&c->lock);
    return result;
}

static int disk_read(struct block_device *block, uint64_t sector, uint32_t count, void *buffer) {
    return disk_transfer(block, sector, count, buffer, false);
}

static int disk_write(struct block_device *block, uint64_t sector, uint32_t count,
                      const void *buffer) {
    return disk_transfer(block, sector, count, (void *)buffer, true);
}

/* ---- CD/DVD drives (ATAPI) ---- */

/* One SCSI command; `length` bytes come back into `buffer`. */
static int packet(struct drive *d, const uint8_t command[12], void *buffer, uint32_t length) {
    struct channel *c = d->channel;
    select_drive(c, d->slave, 0);
    if (wait_ready(c, false, 5000) == 0xff) {
        return -VX_ETIMEDOUT;
    }
    outb((uint16_t)(c->command + REG_FEATURES), 0); /* (PIO.) */
    outb((uint16_t)(c->command + REG_LBA1), 0xfe); /* At most this much at a time. */
    outb((uint16_t)(c->command + REG_LBA2), 0xff);
    outb((uint16_t)(c->command + REG_COMMAND), CMD_PACKET);
    settle(c);
    uint8_t status = wait_ready(c, true, 5000);
    if (status == 0xff || !(status & STATUS_DRQ)) {
        return -VX_EIO;
    }
    rep_outsw((uint16_t)(c->command + REG_DATA), command, 6);
    uint8_t *p = buffer;
    uint32_t got = 0;
    for (;;) {
        settle(c);
        status = wait_ready(c, false, 10000);
        if (status == 0xff) {
            return -VX_ETIMEDOUT;
        }
        if (status & (STATUS_ERROR | STATUS_FAULT)) {
            return -VX_EIO;
        }
        if (!(status & STATUS_DRQ)) {
            break; /* All there is. */
        }
        uint32_t bytes = inb((uint16_t)(c->command + REG_LBA1)) |
                         (uint32_t)inb((uint16_t)(c->command + REG_LBA2)) << 8;
        uint32_t take = bytes < length - got ? bytes : length - got;
        rep_insw((uint16_t)(c->command + REG_DATA), p + got, take / 2);
        for (uint32_t extra = take / 2 * 2; extra < bytes; extra += 2) {
            inw((uint16_t)(c->command + REG_DATA)); /* (More than was asked for.) */
        }
        got += take;
    }
    return (int)got;
}

static int cd_read(struct block_device *block, uint64_t sector, uint32_t count, void *buffer) {
    struct drive *d = (struct drive *)block;
    uint8_t *p = buffer;
    int result = 0;
    mutex_lock(&d->channel->lock);
    while (count && result == 0) {
        uint32_t n = count < 16 ? count : 16; /* (32 KiB: under the byte count limit.) */
        uint8_t command[12] = {SCSI_READ_10, 0, (uint8_t)(sector >> 24), (uint8_t)(sector >> 16),
                               (uint8_t)(sector >> 8), (uint8_t)sector, 0, (uint8_t)(n >> 8),
                               (uint8_t)n, 0, 0, 0};
        int got = packet(d, command, p, n * CD_SECTOR);
        if (got != (int)(n * CD_SECTOR)) {
            result = got < 0 ? got : -VX_EIO;
        }
        p += n * CD_SECTOR;
        sector += n;
        count -= n;
    }
    mutex_unlock(&d->channel->lock);
    return result;
}

/* ---- Finding the drives ---- */

static void add_drive(struct channel *c, bool slave, struct device *controller) {
    if (drive_count == MAX_DRIVES) {
        return;
    }
    select_drive(c, slave, 0);
    if (status_of(c) == 0xff) {
        return; /* Nothing on the channel. */
    }
    outb((uint16_t)(c->command + REG_COUNT), 0);
    outb((uint16_t)(c->command + REG_LBA0), 0);
    outb((uint16_t)(c->command + REG_LBA1), 0);
    outb((uint16_t)(c->command + REG_LBA2), 0);
    outb((uint16_t)(c->command + REG_COMMAND), CMD_IDENTIFY);
    settle(c);
    if (status_of(c) == 0) {
        return; /* No drive there. */
    }
    if (wait_ready(c, false, 3000) == 0xff) {
        return;
    }
    /* An ATAPI drive refuses IDENTIFY, and says what it is here. */
    uint8_t mid = inb((uint16_t)(c->command + REG_LBA1)),
            high = inb((uint16_t)(c->command + REG_LBA2));
    bool atapi = (mid == 0x14 && high == 0xeb) || (mid == 0x69 && high == 0x96);
    if (!atapi && (mid || high)) {
        return; /* Something else (a SATA drive behind a bridge we don't know). */
    }
    if (atapi) {
        outb((uint16_t)(c->command + REG_COMMAND), CMD_IDENTIFY_PACKET);
        settle(c);
    }
    uint8_t status = wait_ready(c, true, 3000);
    if (status == 0xff || !(status & STATUS_DRQ)) {
        return;
    }
    uint16_t identify[256];
    rep_insw((uint16_t)(c->command + REG_DATA), identify, 256);

    struct drive *d = kzalloc(sizeof(*d));
    if (!d) {
        return;
    }
    d->channel = c;
    d->slave = slave;
    d->atapi = atapi;
    d->block.driver_data = d;
    d->block.controller = controller;
    char model[41];
    for (int i = 0; i < 20; i++) { /* (Bytes swapped in each word.) */
        model[2 * i] = (char)(identify[27 + i] >> 8);
        model[2 * i + 1] = (char)identify[27 + i];
    }
    model[40] = '\0';
    for (int i = 39; i >= 0 && model[i] == ' '; i--) {
        model[i] = '\0';
    }

    if (atapi) {
        /* Its size, once the disc is ready (the first commands after a
         * reset report a "unit attention"). */
        static const uint8_t ready[12] = {SCSI_TEST_UNIT_READY};
        static const uint8_t read_capacity[12] = {SCSI_READ_CAPACITY};
        uint8_t capacity[8];
        bool ok = false;
        for (int attempt = 0; attempt < 5 && !ok; attempt++) {
            packet(d, ready, NULL, 0);
            ok = packet(d, read_capacity, capacity, 8) == 8;
        }
        if (!ok) {
            kprintf("[ata] %s: a CD/DVD drive with no disc\n", model);
            kfree(d);
            return;
        }
        uint32_t last = (uint32_t)capacity[0] << 24 | capacity[1] << 16 | capacity[2] << 8 |
                        capacity[3];
        ksnprintf(d->block.name, sizeof(d->block.name), "cd%d", block_new_cd_number());
        d->block.sector_count = (uint64_t)last + 1;
        d->block.sector_size = CD_SECTOR;
        d->block.read = cd_read;
        d->block.description = "CD/DVD drive";
    } else {
        d->lba48 = identify[83] & (1 << 10);
        uint64_t sectors = d->lba48 ? ((uint64_t)identify[100] | (uint64_t)identify[101] << 16 |
                                       (uint64_t)identify[102] << 32 |
                                       (uint64_t)identify[103] << 48)
                                    : ((uint32_t)identify[60] | (uint32_t)identify[61] << 16);
        if (!sectors || !(identify[49] & (1 << 9))) {
            kprintf("[ata] %s: a disk without LBA, left alone\n", model);
            kfree(d);
            return;
        }
        ksnprintf(d->block.name, sizeof(d->block.name), "hd%c", 'a' + disk_count++);
        d->block.sector_count = sectors;
        d->block.sector_size = 512;
        d->block.read = disk_read;
        d->block.write = disk_write;
        d->block.description = "IDE disk";
    }
    drives[drive_count++] = d;
    kprintf("[ata] %s: %s (%s), %s channel %s, polling\n", d->block.name,
            atapi ? "CD/DVD drive" : "disk", model[0] ? model : "no name",
            c->command == 0x1f0 ? "primary" : c->command == 0x170 ? "secondary" : "native",
            slave ? "slave" : "master");
    block_register(&d->block);
}

static void add_channel(uint16_t command, uint16_t control, struct device *controller) {
    struct channel *c = kzalloc(sizeof(*c));
    if (!c) {
        return;
    }
    c->command = command;
    c->control = control;
    c->lock = (struct mutex)MUTEX_INIT;
    outb((uint16_t)(c->control + REG_CONTROL), CONTROL_NO_INTERRUPTS);
    add_drive(c, false, controller);
    add_drive(c, true, controller);
}

static void probe(struct pci_device *pci) {
    pci_enable(pci);
    pci_write16(pci, 0x04, (uint16_t)(pci_read16(pci, 0x04) | 1)); /* I/O space too. */
    pci_claim(pci, "ata", NULL);
    /* Each channel: at the old fixed ports, or (native mode) its BARs. */
    for (int ch = 0; ch < 2; ch++) {
        bool native = pci->prog_if & (1 << (2 * ch));
        uint16_t command = ch ? 0x170 : 0x1f0, control = ch ? 0x376 : 0x3f6;
        if (native) {
            if (!pci->bar_is_io[2 * ch] || !pci->bar_is_io[2 * ch + 1]) {
                continue;
            }
            command = (uint16_t)pci->bar[2 * ch];
            control = (uint16_t)(pci->bar[2 * ch + 1] + 2);
        }
        add_channel(command, control, pci->node);
    }
    device_set_details(pci->node, "IDE, PIO, polled");
}

void ata_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->class_code == 0x01 && pci->subclass == 0x01) {
            probe(pci);
        }
    }
}
