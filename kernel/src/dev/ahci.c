#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/block.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/string.h>

/*
 * AHCI: the standard interface for SATA controllers. Each port with a hard
 * disk becomes sda, sdb..., and each CD/DVD drive cd0, cd1... (read-only,
 * 2048-byte sectors, through ATAPI: SCSI commands in a PACKET command). One
 * command slot per port, one request at a time.
 */

#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0c
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28
#define GHC_AHCI_ENABLE (1U << 31)
#define GHC_INTERRUPTS (1U << 1)

#define PORT_BASE(n) (0x100 + (n) * 0x80)
#define PX_CLB 0x00
#define PX_CLBU 0x04
#define PX_FB 0x08
#define PX_FBU 0x0c
#define PX_IS 0x10
#define PX_IE 0x14
#define PX_CMD 0x18
#define PX_TFD 0x20
#define PX_SIG 0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_CI 0x38

#define CMD_START (1U << 0)
#define CMD_FIS_RECEIVE (1U << 4)
#define CMD_FIS_RUNNING (1U << 14)
#define CMD_LIST_RUNNING (1U << 15)
#define TFD_ERROR 0x01
#define TFD_BUSY 0x80
#define TFD_DRQ 0x08
#define IS_TASK_FILE_ERROR (1U << 30)
#define PORT_INTERRUPTS 0x7d80007f /* Completions and all error conditions. */

#define SIG_SATA_DISK 0x00000101
#define SIG_ATAPI 0xeb140101

#define ATA_IDENTIFY 0xec
#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_PACKET 0xa0

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_READ_CAPACITY 0x25
#define SCSI_READ_10 0x28

#define CD_SECTOR 2048
#define HEADER_ATAPI (1 << 5)

struct __attribute__((packed)) command_header {
    uint16_t flags; /* FIS length in dwords (bits 0-4), write (bit 6)... */
    uint16_t prdt_length;
    uint32_t bytes_transferred;
    uint64_t table;
    uint32_t reserved[4];
};

struct __attribute__((packed)) prdt_entry {
    uint64_t address;
    uint32_t reserved;
    uint32_t byte_count; /* Bytes minus one; bit 31 = interrupt when done. */
};

struct __attribute__((packed)) command_table {
    uint8_t fis[64];
    uint8_t atapi[16];
    uint8_t reserved[48];
    struct prdt_entry prdt[1];
};

struct ahci_port {
    struct block_device block;
    volatile uint32_t *regs;
    struct command_header *headers; /* Command list, 32 slots; we use slot 0. */
    struct command_table *table;
    struct mutex lock;
    struct device_waiter waiter;
};

#define MAX_PORTS 32
static struct ahci_port *ports[MAX_PORTS];
static int port_count, disk_count, cd_count;
static volatile uint32_t *interrupt_hbas[4];
static int interrupt_hba_count;

static uint32_t port_read(struct ahci_port *port, uint32_t reg) {
    return port->regs[reg / 4];
}

static void port_write(struct ahci_port *port, uint32_t reg, uint32_t value) {
    port->regs[reg / 4] = value;
}

static void ahci_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    for (int i = 0; i < interrupt_hba_count; i++) {
        volatile uint32_t *hba = interrupt_hbas[i];
        hba[HBA_IS / 4] = hba[HBA_IS / 4]; /* Acknowledge (per-port bits are cleared below). */
    }
    for (int i = 0; i < port_count; i++) {
        uint32_t status = port_read(ports[i], PX_IS);
        if (status) {
            port_write(ports[i], PX_IS, status);
            device_wake(&ports[i]->waiter);
        }
    }
}

static bool command_done(void *arg) {
    struct ahci_port *port = arg;
    return !(port_read(port, PX_CI) & 1) || (port_read(port, PX_IS) & IS_TASK_FILE_ERROR) ||
           (port_read(port, PX_TFD) & TFD_ERROR);
}

/* Runs an ATA command, or with `packet` an ATAPI one (a SCSI command block
 * in a PACKET command). `bytes` may be 0 (no data). */
static int run_command(struct ahci_port *port, uint8_t command, uint64_t lba, uint16_t count,
                       void *buffer, uint32_t bytes, bool write, const uint8_t *packet) {
    struct command_table *table = port->table;
    memset(table, 0, sizeof(*table));
    uint8_t *fis = table->fis;
    fis[0] = 0x27;  /* Register FIS, host to device. */
    fis[1] = 0x80;  /* This is a command. */
    fis[2] = command;
    if (packet) {
        memcpy(table->atapi, packet, 12);
        fis[3] = bytes ? 1 : 0; /* Features: the data goes by DMA. */
        fis[5] = (uint8_t)bytes; /* Byte count limit (for PIO; harmless with DMA). */
        fis[6] = (uint8_t)(bytes >> 8);
    }
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = packet ? 0 : 0x40; /* LBA addressing. */
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);
    if (bytes) {
        table->prdt[0].address = virt_to_phys(buffer);
        table->prdt[0].byte_count = (bytes - 1) | (1U << 31);
    }

    port->headers[0].flags = 5 | (write ? (1 << 6) : 0) | (packet ? HEADER_ATAPI : 0);
    port->headers[0].prdt_length = bytes ? 1 : 0;
    port->headers[0].bytes_transferred = 0;
    port->headers[0].table = virt_to_phys(table);

    for (int spin = 0; port_read(port, PX_TFD) & (TFD_BUSY | TFD_DRQ); spin++) {
        if (spin > 10000000) {
            return -VX_EIO;
        }
    }
    port_write(port, PX_IS, 0xffffffff);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    port_write(port, PX_CI, 1);
    device_wait(&port->waiter, command_done, port);
    bool failed = (port_read(port, PX_IS) & IS_TASK_FILE_ERROR) || (port_read(port, PX_TFD) & TFD_ERROR);
    return failed ? -VX_EIO : 0;
}

static int ahci_transfer(struct block_device *block, uint64_t sector, uint32_t count, void *buffer,
                         bool write) {
    struct ahci_port *port = (struct ahci_port *)block;
    mutex_lock(&port->lock);
    int result = run_command(port, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, sector,
                             (uint16_t)count, buffer, count * block->sector_size, write, NULL);
    mutex_unlock(&port->lock);
    return result;
}

static int ahci_read(struct block_device *block, uint64_t sector, uint32_t count, void *buffer) {
    return ahci_transfer(block, sector, count, buffer, false);
}

static int ahci_write(struct block_device *block, uint64_t sector, uint32_t count,
                      const void *buffer) {
    return ahci_transfer(block, sector, count, (void *)buffer, true);
}

/* CD/DVD drives: READ(10), up to 65535 sectors at a time. */
static int cd_read(struct block_device *block, uint64_t sector, uint32_t count, void *buffer) {
    struct ahci_port *port = (struct ahci_port *)block;
    uint8_t packet[12] = {SCSI_READ_10, 0, (uint8_t)(sector >> 24), (uint8_t)(sector >> 16),
                          (uint8_t)(sector >> 8), (uint8_t)sector, 0, (uint8_t)(count >> 8),
                          (uint8_t)count, 0, 0, 0};
    mutex_lock(&port->lock);
    int result = run_command(port, ATA_PACKET, 0, 0, buffer, count * CD_SECTOR, false, packet);
    mutex_unlock(&port->lock);
    return result;
}

static bool wait_clear(struct ahci_port *port, uint32_t reg, uint32_t bits) {
    uint64_t deadline = timer_ms() + 500;
    while (port_read(port, reg) & bits) {
        if (timer_ms() > deadline) {
            return false;
        }
        thread_yield();
    }
    return true;
}

static void *dma_page(void) {
    uint64_t phys = pmm_alloc(0);
    if (!phys) {
        return NULL;
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys_to_virt(phys);
}

static void setup_port(volatile uint32_t *hba, int number, bool interrupts) {
    struct ahci_port *port = kzalloc(sizeof(*port));
    if (!port || port_count == MAX_PORTS) {
        kfree(port);
        return;
    }
    port->regs = (volatile uint32_t *)((volatile uint8_t *)hba + PORT_BASE(number));

    uint32_t status = port_read(port, PX_SSTS);
    bool present = (status & 0xf) == 3 && ((status >> 8) & 0xf) == 1;
    uint32_t signature = port_read(port, PX_SIG);
    if (!present || (signature != SIG_SATA_DISK && signature != SIG_ATAPI)) {
        kfree(port);
        return; /* Empty, asleep, or something else. */
    }

    /* Stop the port while we give it memory for its command list. */
    port_write(port, PX_CMD, port_read(port, PX_CMD) & ~CMD_START);
    wait_clear(port, PX_CMD, CMD_LIST_RUNNING);
    port_write(port, PX_CMD, port_read(port, PX_CMD) & ~CMD_FIS_RECEIVE);
    wait_clear(port, PX_CMD, CMD_FIS_RUNNING);

    uint8_t *memory = dma_page(); /* Command list (1 KiB) then received FISes (256 B). */
    port->table = dma_page();
    if (!memory || !port->table) {
        kfree(port);
        return;
    }
    port->headers = (struct command_header *)memory;
    uint64_t list = virt_to_phys(memory), fis = list + 1024;
    port_write(port, PX_CLB, (uint32_t)list);
    port_write(port, PX_CLBU, (uint32_t)(list >> 32));
    port_write(port, PX_FB, (uint32_t)fis);
    port_write(port, PX_FBU, (uint32_t)(fis >> 32));
    port_write(port, PX_SERR, 0xffffffff);
    port_write(port, PX_IS, 0xffffffff);
    port_write(port, PX_IE, interrupts ? PORT_INTERRUPTS : 0);
    port_write(port, PX_CMD, port_read(port, PX_CMD) | CMD_FIS_RECEIVE);
    port_write(port, PX_CMD, port_read(port, PX_CMD) | CMD_START);
    port->waiter.has_interrupt = interrupts;
    ports[port_count++] = port;

    if (signature == SIG_ATAPI) {
        /* A CD/DVD drive: its size, once the disc is ready (the first
         * commands after a reset report a "unit attention"). */
        uint8_t *capacity = dma_page();
        const uint8_t ready[12] = {SCSI_TEST_UNIT_READY};
        const uint8_t read_capacity[12] = {SCSI_READ_CAPACITY};
        bool ok = false;
        for (int attempt = 0; capacity && attempt < 5 && !ok; attempt++) {
            run_command(port, ATA_PACKET, 0, 0, NULL, 0, false, ready);
            ok = run_command(port, ATA_PACKET, 0, 0, capacity, 8, false, read_capacity) == 0;
        }
        uint32_t last = ok ? (uint32_t)capacity[0] << 24 | capacity[1] << 16 | capacity[2] << 8 |
                                 capacity[3]
                           : 0;
        if (capacity) {
            pmm_free(virt_to_phys(capacity), 0);
        }
        if (!ok) {
            kprintf("[ahci] port %d: a CD/DVD drive with no disc\n", number);
            port_count--;
            return;
        }
        ksnprintf(port->block.name, sizeof(port->block.name), "cd%d", cd_count++);
        port->block.sector_count = (uint64_t)last + 1;
        port->block.sector_size = CD_SECTOR;
        port->block.read = cd_read;
        port->block.write = NULL;
        kprintf("[ahci] %s: CD/DVD drive on port %d, %s\n", port->block.name, number,
                interrupts ? "MSI interrupts" : "polling");
        block_register(&port->block);
        return;
    }

    uint16_t *identify = dma_page();
    if (!identify || run_command(port, ATA_IDENTIFY, 0, 0, identify, 512, false, NULL) != 0) {
        kprintf("[ahci] port %d: IDENTIFY failed\n", number);
        port_count--;
        return;
    }
    uint64_t sectors = (uint64_t)identify[100] | (uint64_t)identify[101] << 16 |
                       (uint64_t)identify[102] << 32 | (uint64_t)identify[103] << 48;
    if (!sectors) {
        sectors = (uint32_t)identify[60] | (uint32_t)identify[61] << 16; /* No LBA48. */
    }
    uint32_t sector_size = 512;
    if ((identify[106] & 0xc000) == 0x4000 && (identify[106] & (1 << 12))) {
        sector_size = 2 * ((uint32_t)identify[117] | (uint32_t)identify[118] << 16);
    }
    pmm_free(virt_to_phys(identify), 0);

    memcpy(port->block.name, "sda", 4);
    port->block.name[2] = (char)('a' + disk_count++);
    port->block.sector_count = sectors;
    port->block.sector_size = sector_size;
    port->block.read = ahci_read;
    port->block.write = ahci_write;
    kprintf("[ahci] %s: SATA disk on port %d, %s\n", port->block.name, number,
            interrupts ? "MSI interrupts" : "polling");
    block_register(&port->block);
}

static void probe(struct pci_device *pci) {
    pci_enable(pci);
    volatile uint32_t *hba = pci_map_bar(pci, 5);
    if (!hba) {
        return;
    }
    /* Take the controller over from the firmware, if it asks for a handoff. */
    if (hba[HBA_CAP2 / 4] & 1) {
        hba[HBA_BOHC / 4] |= 1 << 1;
        uint64_t deadline = timer_ms() + 100;
        while ((hba[HBA_BOHC / 4] & 1) && timer_ms() < deadline) {
            thread_yield();
        }
    }
    hba[HBA_GHC / 4] |= GHC_AHCI_ENABLE;

    bool interrupts = interrupt_hba_count < 4 && pci_enable_msi(pci, ahci_interrupt);
    if (interrupts) {
        interrupt_hbas[interrupt_hba_count++] = hba;
        hba[HBA_IS / 4] = 0xffffffff;
        hba[HBA_GHC / 4] |= GHC_INTERRUPTS;
    }
    uint32_t implemented = hba[HBA_PI / 4];
    for (int port = 0; port < 32; port++) {
        if (implemented & (1U << port)) {
            setup_port(hba, port, interrupts);
        }
    }
}

void ahci_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->class_code == 0x01 && pci->subclass == 0x06 && pci->prog_if == 0x01) {
            probe(pci);
        }
    }
}
