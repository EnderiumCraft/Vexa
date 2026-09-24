#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/block.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/string.h>

/*
 * NVMe: the interface of modern SSDs. We use the admin queue to set things up
 * and one I/O queue pair for reads and writes, one command at a time, each at
 * most one page (the block cache never asks for more). The first namespace
 * becomes nvme0n1.
 */

#define REG_CAP 0x00
#define REG_CC 0x14
#define REG_CSTS 0x1c
#define REG_AQA 0x24
#define REG_ASQ 0x28
#define REG_ACQ 0x30
#define DOORBELLS 0x1000

#define CC_ENABLE 1
#define CC_IO_QUEUE_SIZES ((6U << 16) | (4U << 20)) /* 64-byte commands, 16-byte completions. */
#define CSTS_READY 1
#define CSTS_FATAL 2

#define ADMIN_CREATE_IO_SQ 0x01
#define ADMIN_CREATE_IO_CQ 0x05
#define ADMIN_IDENTIFY 0x06
#define IO_WRITE 0x01
#define IO_READ 0x02

#define QUEUE_ENTRIES 16

struct __attribute__((packed)) nvme_command {
    uint8_t opcode;
    uint8_t flags;
    uint16_t id;
    uint32_t namespace_id;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct __attribute__((packed)) nvme_completion {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t id;
    uint16_t status; /* Bit 0: phase; the rest: status code (0 = success). */
};

struct queue {
    struct nvme_command *commands;
    volatile struct nvme_completion *completions;
    volatile uint32_t *sq_doorbell;
    volatile uint32_t *cq_doorbell;
    uint16_t tail, head;
    uint16_t phase;
};

struct nvme {
    struct block_device block;
    volatile uint8_t *regs;
    struct queue admin, io;
    uint32_t namespace_id;
    uint16_t next_id;
    struct mutex lock;
    struct device_waiter waiter;
};

#define MAX_CONTROLLERS 4
static struct nvme *controllers[MAX_CONTROLLERS];
static int controller_count;

static uint64_t read64(struct nvme *nvme, uint32_t reg) {
    return *(volatile uint64_t *)(nvme->regs + reg);
}

static uint32_t read32(struct nvme *nvme, uint32_t reg) {
    return *(volatile uint32_t *)(nvme->regs + reg);
}

static void write32(struct nvme *nvme, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(nvme->regs + reg) = value;
}

static void write64(struct nvme *nvme, uint32_t reg, uint64_t value) {
    write32(nvme, reg, (uint32_t)value);
    write32(nvme, reg + 4, (uint32_t)(value >> 32));
}

static void nvme_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    for (int i = 0; i < controller_count; i++) {
        device_wake(&controllers[i]->waiter);
    }
}

static void *dma_page(void) {
    uint64_t phys = pmm_alloc(0);
    if (!phys) {
        return NULL;
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys_to_virt(phys);
}

struct wait_args {
    struct queue *queue;
};

static bool completion_ready(void *arg) {
    struct queue *queue = ((struct wait_args *)arg)->queue;
    return (queue->completions[queue->head].status & 1) == queue->phase;
}

/* Submits a command and waits for its completion. Returns 0 or -VX_EIO. */
static int execute(struct nvme *nvme, struct queue *queue, struct nvme_command *command,
                   bool use_interrupt) {
    command->id = nvme->next_id++;
    queue->commands[queue->tail] = *command;
    queue->tail = (queue->tail + 1) % QUEUE_ENTRIES;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *queue->sq_doorbell = queue->tail;

    struct wait_args args = {queue};
    if (use_interrupt) {
        device_wait(&nvme->waiter, completion_ready, &args);
    } else {
        uint64_t deadline = timer_ms() + 5000;
        while (!completion_ready(&args)) {
            if (timer_ms() > deadline) {
                return -VX_EIO;
            }
            thread_yield();
        }
    }
    uint16_t status = queue->completions[queue->head].status >> 1;
    queue->head = (queue->head + 1) % QUEUE_ENTRIES;
    if (queue->head == 0) {
        queue->phase ^= 1; /* The completion ring wrapped: new entries flip the phase bit. */
    }
    *queue->cq_doorbell = queue->head;
    return status ? -VX_EIO : 0;
}

static int nvme_transfer(struct block_device *block, uint64_t sector, uint32_t count, void *buffer,
                         bool write) {
    struct nvme *nvme = (struct nvme *)block;
    struct nvme_command command = {
        .opcode = write ? IO_WRITE : IO_READ,
        .namespace_id = nvme->namespace_id,
        .prp1 = virt_to_phys(buffer),
        .cdw10 = (uint32_t)sector,
        .cdw11 = (uint32_t)(sector >> 32),
        .cdw12 = count - 1,
    };
    /* A transfer crossing into a second page names it in PRP2. */
    uint64_t end = command.prp1 + (uint64_t)count * block->sector_size;
    if ((command.prp1 & ~(PAGE_SIZE - 1)) != ((end - 1) & ~(PAGE_SIZE - 1))) {
        command.prp2 = (command.prp1 & ~(PAGE_SIZE - 1)) + PAGE_SIZE;
    }
    mutex_lock(&nvme->lock);
    int result = execute(nvme, &nvme->io, &command, nvme->waiter.has_interrupt);
    mutex_unlock(&nvme->lock);
    return result;
}

static int nvme_read(struct block_device *block, uint64_t sector, uint32_t count, void *buffer) {
    return nvme_transfer(block, sector, count, buffer, false);
}

static int nvme_write(struct block_device *block, uint64_t sector, uint32_t count,
                      const void *buffer) {
    return nvme_transfer(block, sector, count, (void *)buffer, true);
}

static bool wait_ready(struct nvme *nvme, bool ready) {
    uint64_t timeout = ((read64(nvme, REG_CAP) >> 24) & 0xff) * 500 + 500;
    uint64_t deadline = timer_ms() + timeout;
    while (((read32(nvme, REG_CSTS) & CSTS_READY) != 0) != ready) {
        if (timer_ms() > deadline || (read32(nvme, REG_CSTS) & CSTS_FATAL)) {
            return false;
        }
        thread_yield();
    }
    return true;
}

static bool setup_queue(struct nvme *nvme, struct queue *queue, int id, uint32_t stride) {
    queue->commands = dma_page();
    queue->completions = dma_page();
    queue->sq_doorbell = (volatile uint32_t *)(nvme->regs + DOORBELLS + (2 * id) * stride);
    queue->cq_doorbell = (volatile uint32_t *)(nvme->regs + DOORBELLS + (2 * id + 1) * stride);
    queue->phase = 1;
    return queue->commands && queue->completions;
}

static void probe(struct pci_device *pci) {
    if (controller_count == MAX_CONTROLLERS) {
        return;
    }
    pci_enable(pci);
    struct nvme *nvme = kzalloc(sizeof(*nvme));
    if (!nvme || !(nvme->regs = pci_map_bar(pci, 0))) {
        kfree(nvme);
        return;
    }
    uint64_t cap = read64(nvme, REG_CAP);
    uint32_t stride = 4U << ((cap >> 32) & 0xf);
    if (((cap >> 48) & 0xf) != 0) {
        kprintf("[nvme] controller doesn't support 4 KiB pages, skipping\n");
        kfree(nvme);
        return;
    }

    /* Disable, set up the admin queues, enable. */
    write32(nvme, REG_CC, read32(nvme, REG_CC) & ~CC_ENABLE);
    if (!wait_ready(nvme, false) || !setup_queue(nvme, &nvme->admin, 0, stride) ||
        !setup_queue(nvme, &nvme->io, 1, stride)) {
        kprintf("[nvme] controller didn't stop, skipping\n");
        kfree(nvme);
        return;
    }
    write32(nvme, REG_AQA, (QUEUE_ENTRIES - 1) << 16 | (QUEUE_ENTRIES - 1));
    write64(nvme, REG_ASQ, virt_to_phys(nvme->admin.commands));
    write64(nvme, REG_ACQ, virt_to_phys((void *)nvme->admin.completions));
    write32(nvme, REG_CC, CC_IO_QUEUE_SIZES | CC_ENABLE);
    if (!wait_ready(nvme, true)) {
        kprintf("[nvme] controller didn't start, skipping\n");
        kfree(nvme);
        return;
    }

    controllers[controller_count++] = nvme;
    bool interrupts = pci_enable_msi(pci, nvme_interrupt);

    /* The I/O queue pair: completions first, then commands feeding them. */
    struct nvme_command command = {
        .opcode = ADMIN_CREATE_IO_CQ,
        .prp1 = virt_to_phys((void *)nvme->io.completions),
        .cdw10 = (QUEUE_ENTRIES - 1) << 16 | 1,
        .cdw11 = 1 | (interrupts ? 2 : 0), /* Physically contiguous; interrupts on vector 0. */
    };
    bool ok = execute(nvme, &nvme->admin, &command, false) == 0;
    command = (struct nvme_command){
        .opcode = ADMIN_CREATE_IO_SQ,
        .prp1 = virt_to_phys(nvme->io.commands),
        .cdw10 = (QUEUE_ENTRIES - 1) << 16 | 1,
        .cdw11 = 1U << 16 | 1, /* Completion queue 1; physically contiguous. */
    };
    ok = ok && execute(nvme, &nvme->admin, &command, false) == 0;

    /* Identify namespace 1: its size and sector size. */
    uint8_t *identify = dma_page();
    command = (struct nvme_command){
        .opcode = ADMIN_IDENTIFY, .namespace_id = 1,
        .prp1 = identify ? virt_to_phys(identify) : 0, .cdw10 = 0};
    ok = ok && identify && execute(nvme, &nvme->admin, &command, false) == 0;
    if (!ok) {
        kprintf("[nvme] setting up the controller failed\n");
        return;
    }
    uint64_t sectors = *(uint64_t *)identify;
    uint8_t format = identify[26] & 0xf;
    uint32_t lba_format = *(uint32_t *)(identify + 128 + format * 4);
    uint32_t sector_size = 1U << ((lba_format >> 16) & 0xff);
    pmm_free(virt_to_phys(identify), 0);
    if (!sectors || sector_size < 512 || sector_size > PAGE_SIZE) {
        kprintf("[nvme] namespace 1 is missing or unusable\n");
        return;
    }

    nvme->namespace_id = 1;
    nvme->waiter.has_interrupt = interrupts;
    int index = controller_count - 1;
    memcpy(nvme->block.name, "nvme0n1", 8);
    nvme->block.name[4] = (char)('0' + index);
    nvme->block.sector_count = sectors;
    nvme->block.sector_size = sector_size;
    nvme->block.read = nvme_read;
    nvme->block.write = nvme_write;
    kprintf("[nvme] %s: NVMe drive, %s\n", nvme->block.name,
            interrupts ? "MSI-X interrupts" : "polling");
    block_register(&nvme->block);
}

void nvme_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->class_code == 0x01 && pci->subclass == 0x08 && pci->prog_if == 0x02) {
            probe(pci);
        }
    }
}
