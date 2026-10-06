#include <vexa/device.h>
#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/sched.h>
#include <vexa/sound.h>
#include <vexa/string.h>

/*
 * AC'97: Intel's ICH sound (82801AA and later, 8086:2415 and kin), and what
 * VirtualBox gives many guests (and QEMU's "AC97"). Two sets of I/O ports:
 * the codec's mixer (NAM: volumes, the sample rate), and the controller's bus
 * master (NABM), which plays by DMA from a list of 32 buffers (the BDL),
 * going round them while the "last valid index" stays ahead of where it is.
 *
 * Like the HD Audio driver: one output, 16-bit stereo, from a 64 KiB ring
 * cut into the 32 buffers; a kernel thread follows where the controller is
 * (no interrupts needed), keeps the ring filled a little ahead with what the
 * sound core mixes, and stops after a while of silence.
 */

/* The mixer (NAM). */
#define MIX_RESET 0x00
#define MIX_MASTER 0x02
#define MIX_HEADPHONE 0x04
#define MIX_PCM_OUT 0x18
#define MIX_EXT_ID 0x28
#define MIX_EXT_CTRL 0x2a
#define MIX_FRONT_RATE 0x2c
#define EXT_VRA 1 /* Variable rate. */

/* The bus master (NABM): PCM out's registers, and the global ones. */
#define PO_BDBAR 0x10
#define PO_CIV 0x14 /* The buffer being played. */
#define PO_LVI 0x15 /* The last one it may play. */
#define PO_SR 0x16
#define PO_PICB 0x18 /* Samples left in the current buffer. */
#define PO_CR 0x1b
#define GLOB_CNT 0x2c
#define GLOB_STA 0x30
#define CR_RUN (1u << 0)
#define CR_RESET (1u << 1)
#define SR_CLEAR 0x1c /* (Written as 1s.) */
#define CNT_COLD (1u << 1) /* Out of cold reset. */
#define STA_READY (1u << 8) /* The primary codec. */

#define BUFFERS 32
#define RING_SIZE (64 * 1024) /* Order 4: 16 pages. */
#define PIECE (RING_SIZE / BUFFERS)
#define TICK_MS 5
#define LEAD_MS 100 /* Kept filled ahead of the controller. */
#define IDLE_MS 2000 /* Stops after this much silence. */

struct bdl_entry {
    uint32_t address;
    uint16_t samples; /* 16-bit ones (both channels). */
    uint16_t flags;
};

static uint16_t nam, nabm;
static uint8_t *ring;
static struct bdl_entry *bdl;
static struct mutex lock = MUTEX_INIT;
static uint64_t written, played;
static uint32_t last_position;
static bool running;
static volatile bool want_start;
static uint64_t last_sound;
static struct sound_output output;

static uint64_t lead_bytes(void) {
    return (uint64_t)output.rate * 4 * LEAD_MS / 1000;
}

static uint64_t delay_bytes(void) {
    return written > played ? written - played : 0;
}

/* Where in the ring the controller is playing. */
static uint32_t position(void) {
    unsigned index = inb((uint16_t)(nabm + PO_CIV)) % BUFFERS;
    unsigned left = inw((uint16_t)(nabm + PO_PICB)) * 2u;
    left = left > PIECE ? PIECE : left;
    return (uint32_t)(index * PIECE + PIECE - left) % RING_SIZE;
}

/* With the lock: follows the controller, silences what it has played, and
 * keeps the last valid buffer just behind it (so it never stops). */
static void advance(void) {
    if (!running) {
        return;
    }
    uint32_t now = position();
    uint32_t moved = (now + RING_SIZE - last_position) % RING_SIZE;
    for (uint32_t i = 0; i < moved; i++) {
        ring[(last_position + i) % RING_SIZE] = 0;
    }
    last_position = now;
    played += moved;
    if (written < played) { /* Ran out: carry on from here. */
        written = played;
    }
    unsigned current = inb((uint16_t)(nabm + PO_CIV)) % BUFFERS;
    outb((uint16_t)(nabm + PO_LVI), (uint8_t)((current + BUFFERS - 1) % BUFFERS));
}

static void start(void) {
    memset(ring, 0, RING_SIZE);
    outb((uint16_t)(nabm + PO_CR), CR_RESET);
    for (int tries = 0; tries < 100 && (inb((uint16_t)(nabm + PO_CR)) & CR_RESET); tries++) {
        thread_yield();
    }
    outl((uint16_t)(nabm + PO_BDBAR), (uint32_t)virt_to_phys(bdl));
    outb((uint16_t)(nabm + PO_LVI), BUFFERS - 1);
    outw((uint16_t)(nabm + PO_SR), SR_CLEAR);
    outb((uint16_t)(nabm + PO_CR), CR_RUN);
    written = played = 0;
    last_position = 0;
    running = true;
}

static void stop(void) {
    outb((uint16_t)(nabm + PO_CR), 0);
    running = false;
    written = played = 0;
}

/* With the lock: fills the ring up to the lead ahead of the controller. */
static void fill(void) {
    static int16_t mixed[1024 * 2];
    uint64_t lead = lead_bytes();
    while (delay_bytes() + 4 <= lead) {
        unsigned frames = (unsigned)((lead - delay_bytes()) / 4);
        frames = frames < 1024 ? frames : 1024;
        if (sound_mix(&output, mixed, frames)) {
            last_sound = timer_ms();
        }
        const uint8_t *from = (const uint8_t *)mixed;
        for (unsigned i = 0; i < frames * 4; i++) {
            ring[(written + i) % RING_SIZE] = from[i];
        }
        written += frames * 4;
    }
}

static void ticker(void *arg) {
    (void)arg;
    for (;;) {
        thread_sleep_ms(TICK_MS);
        mutex_lock(&lock);
        if (want_start && !running) {
            start();
            last_sound = timer_ms();
        }
        want_start = false;
        if (running) {
            advance();
            fill();
            if (timer_ms() - last_sound > IDLE_MS && delay_bytes() == 0) {
                stop();
            }
        }
        output.queued = running ? (unsigned)(delay_bytes() / 4) : 0;
        mutex_unlock(&lock);
    }
}

/* The core has something to play (with its lock held: just take note). */
static void ac97_start(struct sound_output *o) {
    (void)o;
    want_start = true;
}

/* ---- Bringing it up ---- */

static bool probe(struct pci_device *pci) {
    if (!pci->bar_is_io[0] || !pci->bar_is_io[1]) {
        return false;
    }
    pci_enable(pci);
    pci_write16(pci, 0x04, pci_read16(pci, 0x04) | 1); /* Command: I/O space on. */
    nam = (uint16_t)pci->bar[0];
    nabm = (uint16_t)pci->bar[1];
    /* Out of cold reset; the codec says when it's ready. */
    outl((uint16_t)(nabm + GLOB_CNT), CNT_COLD);
    uint64_t deadline = timer_ms() + 1000;
    while (!(inl((uint16_t)(nabm + GLOB_STA)) & STA_READY)) {
        if (timer_ms() > deadline) {
            kprintf("[ac97] the codec isn't ready\n");
            return false;
        }
        thread_sleep_ms(1);
    }
    outw((uint16_t)(nam + MIX_RESET), 0);
    thread_sleep_ms(1);
    outw((uint16_t)(nam + MIX_MASTER), 0);     /* Loudest, not muted (the core sets the volume). */
    outw((uint16_t)(nam + MIX_HEADPHONE), 0);
    outw((uint16_t)(nam + MIX_PCM_OUT), 0x0808); /* 0 dB. */
    /* 48 kHz, if the rate can be chosen (it's that otherwise). */
    unsigned rate = 48000;
    if (inw((uint16_t)(nam + MIX_EXT_ID)) & EXT_VRA) {
        outw((uint16_t)(nam + MIX_EXT_CTRL), (uint16_t)(inw((uint16_t)(nam + MIX_EXT_CTRL)) | EXT_VRA));
        outw((uint16_t)(nam + MIX_FRONT_RATE), 48000);
        unsigned got = inw((uint16_t)(nam + MIX_FRONT_RATE));
        rate = got >= 8000 ? got : 48000;
    }
    ring = NULL;
    uint64_t ring_phys = pmm_alloc(4), bdl_phys = pmm_alloc(0);
    if (!ring_phys || !bdl_phys || ring_phys + RING_SIZE > 0x100000000ULL ||
        bdl_phys + PAGE_SIZE > 0x100000000ULL) {
        kprintf("[ac97] no memory below 4 GiB for its buffers\n");
        return false;
    }
    ring = phys_to_virt(ring_phys);
    bdl = phys_to_virt(bdl_phys);
    memset(ring, 0, RING_SIZE);
    memset(bdl, 0, PAGE_SIZE);
    for (int i = 0; i < BUFFERS; i++) {
        bdl[i].address = (uint32_t)(ring_phys + (uint64_t)i * PIECE);
        bdl[i].samples = PIECE / 2;
        bdl[i].flags = 0;
    }
    output.rate = rate;
    kprintf("[ac97] %04x:%04x, %u Hz\n", pci->vendor_id, pci->device_id, rate);
    return true;
}

/* Controllers with the ICH's registers: Intel's, and AMD's and nVidia's like them. */
static const struct {
    uint16_t vendor, device;
} known[] = {
    {0x8086, 0x2415}, {0x8086, 0x2425}, {0x8086, 0x2445}, {0x8086, 0x2485}, {0x8086, 0x24c5},
    {0x8086, 0x24d5}, {0x8086, 0x25a6}, {0x8086, 0x266e}, {0x8086, 0x27de}, {0x8086, 0x7195},
    {0x1022, 0x7445}, {0x1022, 0x746d}, {0x10de, 0x01b1}, {0x10de, 0x006a}, {0x10de, 0x00da},
    {0x10de, 0x00ea}, {0x10de, 0x008a}, {0x10de, 0x00e5}, {0x10de, 0x0059},
};

static bool is_known(const struct pci_device *pci) {
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (pci->vendor_id == known[i].vendor && pci->device_id == known[i].device) {
            return true;
        }
    }
    return false;
}

void ac97_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (is_known(pci) && probe(pci)) {
            pci_claim(pci, "ac97", NULL);
            device_set_details(pci->node, "/dev/audio0, %u Hz", output.rate);
            memcpy(output.name, "Built-in (AC'97)", 17);
            output.start = ac97_start;
            thread_create("ac97", ticker, NULL);
            sound_register(&output);
            return;
        }
    }
}
