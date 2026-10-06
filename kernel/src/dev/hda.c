#include <vexa/device.h>
#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/sched.h>
#include <vexa/sound.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * Intel High Definition Audio: the sound hardware of most PCs since 2004,
 * QEMU's intel-hda (with hda-output or hda-duplex) and VirtualBox's (with a
 * STAC9220 codec). The controller talks to codecs over a link, by commands
 * ("verbs") through two rings in memory (CORB out, RIRB back); sound goes out
 * by DMA from a buffer described by a list of pieces (the BDL), which the
 * controller goes round and round.
 *
 * Here: the first codec's first output path (a DAC, through mixers or
 * selectors, to a pin that can drive a speaker or headphones), and one output
 * stream from a 64 KiB ring, at 48 kHz, 16-bit stereo. It's an output of the
 * sound core (dev/sound.c): a kernel thread follows where the controller is
 * playing (its position register, so no interrupts are needed), keeps the
 * ring filled a little ahead of that with what the core mixes, and silences
 * what has played; after a while of silence it stops the stream.
 */

#define GCAP 0x00
#define GCTL 0x08
#define STATESTS 0x0e
#define INTCTL 0x20
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP 0x48
#define CORBRP 0x4a
#define CORBCTL 0x4c
#define CORBSIZE 0x4e
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP 0x58
#define RINTCNT 0x5a
#define RIRBCTL 0x5c
#define RIRBSTS 0x5d
#define RIRBSIZE 0x5e

#define SD_CTL 0x00 /* (3 bytes; the stream's tag is in the top byte's high nibble) */
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0c
#define SD_FMT 0x12
#define SD_BDPL 0x18
#define SD_BDPU 0x1c
#define SD_RESET (1u << 0)
#define SD_RUN (1u << 1)
#define STREAM_TAG 1

/* Verbs (12-bit ones with an 8-bit payload, 4-bit ones with 16 bits). */
#define GET_PARAMETER 0xf00
#define GET_CONNECTIONS 0xf02
#define GET_CONFIG 0xf1c
#define SET_SELECT 0x701
#define SET_POWER 0x705
#define SET_STREAM 0x706
#define SET_PIN 0x707
#define SET_EAPD 0x70c
#define SET_FORMAT 0x2
#define SET_AMP 0x3
#define PARAM_NODES 0x04
#define PARAM_GROUP 0x05
#define PARAM_WIDGET 0x09
#define PARAM_PIN 0x0c
#define PARAM_IN_AMP 0x0d
#define PARAM_CONNECTIONS 0x0e
#define PARAM_OUT_AMP 0x12
#define WIDGET_OUTPUT 0
#define WIDGET_MIXER 2
#define WIDGET_SELECTOR 3
#define WIDGET_PIN 4

#define RING_SIZE (64 * 1024) /* Order 4: 16 pages. */
#define PIECES 4
#define GUARD 4096 /* Never written ahead of: the controller may be reading there. */
#define TICK_MS 5
#define LEAD (100 * 48 * 4) /* Bytes kept filled ahead of the controller: 100 ms. */
#define IDLE_MS 2000 /* Stops after this much silence. */

struct bdl_entry {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
};

static volatile uint8_t *regs;
static volatile uint8_t *stream; /* The output stream's descriptor. */
static uint32_t *corb;
static uint64_t *rirb;
static unsigned rirb_read;
static uint8_t codec;
static uint8_t *ring;
static struct mutex lock = MUTEX_INIT;

/* The path: DAC, then the widgets after it up to the pin. */
static uint8_t path[8];
static uint8_t path_input[8]; /* Which of path[i]'s inputs leads to path[i + 1]. */
static int path_length;
static uint8_t afg; /* The audio function group: amplifiers' defaults. */

/* Bytes written and played since the stream started (both only grow). */
static uint64_t written, played;
static uint32_t last_position;
static bool running;
static volatile bool want_start;
static uint64_t last_sound; /* When the core last had something to play. */
static struct sound_output output;

static uint8_t r8(unsigned at) { return regs[at]; }
static uint16_t r16(unsigned at) { return *(volatile uint16_t *)(regs + at); }
static uint32_t r32(unsigned at) { return *(volatile uint32_t *)(regs + at); }
static void w8(unsigned at, uint8_t v) { regs[at] = v; }
static void w16(unsigned at, uint16_t v) { *(volatile uint16_t *)(regs + at) = v; }
static void w32(unsigned at, uint32_t v) { *(volatile uint32_t *)(regs + at) = v; }

static bool wait_for(unsigned at, uint32_t mask, uint32_t want, bool wide) {
    uint64_t deadline = timer_ms() + 200;
    while (((wide ? r32(at) : r8(at)) & mask) != want) {
        if (timer_ms() > deadline) {
            return false;
        }
        thread_yield();
    }
    return true;
}

/* Sends a command to the codec and returns its answer (or ~0 if none came). */
static uint32_t command(uint8_t node, uint32_t verb, uint32_t payload) {
    uint32_t word = (uint32_t)codec << 28 | (uint32_t)node << 20 |
                    (verb > 0xf ? verb << 8 | (payload & 0xff) : verb << 16 | (payload & 0xffff));
    unsigned wp = (r16(CORBWP) + 1) & 0xff;
    corb[wp] = word;
    w16(CORBWP, (uint16_t)wp);
    uint64_t deadline = timer_ms() + 200;
    while ((r16(RIRBWP) & 0xff) == rirb_read) {
        if (timer_ms() > deadline) {
            return ~0u;
        }
        thread_yield();
    }
    rirb_read = (rirb_read + 1) & 0xff;
    w8(RIRBSTS, 0x5); /* Acknowledged (QEMU's stops answering after RINTCNT otherwise). */
    return (uint32_t)rirb[rirb_read];
}

static uint32_t parameter(uint8_t node, uint32_t id) {
    return command(node, GET_PARAMETER, id);
}

static unsigned widget_type(uint8_t node) {
    return (parameter(node, PARAM_WIDGET) >> 20) & 0xf;
}

/* The nodes a widget takes its input from. */
static int connections(uint8_t node, uint8_t *out, int max) {
    uint32_t info = parameter(node, PARAM_CONNECTIONS);
    int count = (int)(info & 0x7f), n = 0;
    if (info & 0x80) {
        return 0; /* Long form (16-bit entries): not on the codecs we know. */
    }
    for (int i = 0; i < count && n < max; i += 4) {
        uint32_t entries = command(node, GET_CONNECTIONS, (uint32_t)i);
        for (int j = 0; j < 4 && i + j < count && n < max; j++) {
            out[n++] = (uint8_t)(entries >> (8 * j));
        }
    }
    return n;
}

/* Depth-first from `node` towards a DAC; fills path[] backwards. */
static bool find_dac(uint8_t node, int depth) {
    unsigned type = widget_type(node);
    path[depth] = node;
    if (type == WIDGET_OUTPUT) {
        path_length = depth + 1;
        return true;
    }
    if (depth + 1 >= (int)sizeof(path) || (depth > 0 && type != WIDGET_MIXER &&
                                           type != WIDGET_SELECTOR)) {
        return false;
    }
    uint8_t inputs[16];
    int n = connections(node, inputs, 16);
    for (int i = 0; i < n; i++) {
        if (find_dac(inputs[i], depth + 1)) {
            path_input[depth] = (uint8_t)i;
            if (n > 1 && type != WIDGET_MIXER) {
                command(node, SET_SELECT, (uint32_t)i);
            }
            return true;
        }
    }
    return false;
}

/* An amplifier's capabilities: the widget's own, or (without the override
 * bit, as on many codecs, VirtualBox's STAC9220 among them) the function
 * group's. */
static uint32_t amp_caps(uint8_t node, uint32_t which) {
    uint32_t widget = parameter(node, PARAM_WIDGET);
    return widget & (1u << 3) ? parameter(node, which) : parameter(afg, which);
}

/* 0 dB: the "offset" step (or the loudest, if that's 0 and there are steps). */
static uint32_t zero_db(uint32_t caps) {
    uint32_t offset = caps & 0x7f, steps = (caps >> 8) & 0x7f;
    return offset ? offset : steps;
}

/* Turns up (not muted, 0 dB) the node's output amplifier, and the input one
 * on the input the path takes. */
static void unmute(uint8_t node, uint8_t input) {
    uint32_t widget = parameter(node, PARAM_WIDGET);
    if (widget & (1u << 2)) { /* An output amplifier. */
        command(node, SET_AMP, 0xb000 | zero_db(amp_caps(node, PARAM_OUT_AMP)));
    }
    if (widget & (1u << 1)) { /* Input ones. */
        command(node, SET_AMP, 0x7000 | (uint32_t)input << 8 |
                                   zero_db(amp_caps(node, PARAM_IN_AMP)));
    }
}

static uint16_t format_word(void) {
    return (uint16_t)(1u << 4 | 1u); /* 48 kHz, 16-bit, stereo */
}

/* ---- The ring ---- */

static uint64_t delay_bytes(void) {
    return written > played ? written - played : 0;
}

/* With the lock: follows the controller, and silences what it has played. */
static void advance(void) {
    if (!running) {
        return;
    }
    uint32_t position = *(volatile uint32_t *)(stream + SD_LPIB) % RING_SIZE;
    uint32_t moved = (position + RING_SIZE - last_position) % RING_SIZE;
    for (uint32_t i = 0; i < moved; i++) {
        ring[(last_position + i) % RING_SIZE] = 0;
    }
    last_position = position;
    played += moved;
    if (written < played) { /* Ran out: carry on from here. */
        written = played;
    }
}

static void start(void) {
    memset(ring, 0, RING_SIZE);
    *(volatile uint8_t *)(stream + SD_CTL) = 0;
    *(volatile uint16_t *)(stream + SD_FMT) = format_word();
    command(path[path_length - 1], SET_FORMAT, format_word());
    *(volatile uint8_t *)(stream + SD_CTL + 2) = STREAM_TAG << 4;
    *(volatile uint8_t *)(stream + SD_STS) = 0x1c; /* Clear its status. */
    *(volatile uint8_t *)(stream + SD_CTL) = SD_RUN;
    written = played = 0;
    last_position = 0;
    running = true;
}

static void stop(void) {
    *(volatile uint8_t *)(stream + SD_CTL) = 0;
    running = false;
    written = played = 0;
}

/* With the lock: fills the ring up to LEAD ahead of the controller. */
static void fill(void) {
    static int16_t mixed[1024 * 2];
    while (delay_bytes() + 4 <= LEAD) {
        unsigned frames = (unsigned)((LEAD - delay_bytes()) / 4);
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
static void hda_start(struct sound_output *o) {
    (void)o;
    want_start = true;
}

/* ---- Bringing it up ---- */

static void *dma_pages(unsigned order) {
    uint64_t phys = pmm_alloc(order);
    if (!phys) {
        return NULL;
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE << order);
    return phys_to_virt(phys);
}

static bool setup_rings(void) {
    uint8_t *page = dma_pages(1); /* CORB (1 KiB), then RIRB (2 KiB). */
    if (!page) {
        return false;
    }
    corb = (uint32_t *)page;
    rirb = (uint64_t *)(page + 2048);
    uint64_t corb_phys = virt_to_phys(corb), rirb_phys = virt_to_phys(rirb);
    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    wait_for(CORBCTL, 2, 0, false);
    wait_for(RIRBCTL, 2, 0, false);
    w32(CORBLBASE, (uint32_t)corb_phys);
    w32(CORBUBASE, (uint32_t)(corb_phys >> 32));
    w8(CORBSIZE, 2); /* 256 entries. */
    w16(CORBRP, 0x8000); /* Reset its read pointer. */
    uint64_t deadline = timer_ms() + 100;
    while (!(r16(CORBRP) & 0x8000) && timer_ms() < deadline) {
        thread_yield();
    }
    w16(CORBRP, 0);
    w16(CORBWP, 0);
    w32(RIRBLBASE, (uint32_t)rirb_phys);
    w32(RIRBUBASE, (uint32_t)(rirb_phys >> 32));
    w8(RIRBSIZE, 2);
    w16(RIRBWP, 0x8000); /* Reset its write pointer. */
    w16(RINTCNT, 0xff);
    rirb_read = 0;
    w8(CORBCTL, 2); /* Run. */
    w8(RIRBCTL, 2);
    return true;
}

/* The audio function group of the codec, its widgets, and a path to play on. */
static bool setup_codec(void) {
    uint32_t root = parameter(0, PARAM_NODES);
    if (root == ~0u) {
        kprintf("[hda] codec %u doesn't answer\n", codec);
        return false;
    }
    afg = 0;
    for (unsigned n = (root >> 16) & 0xff, i = 0; i < (root & 0xff); i++) {
        if ((parameter((uint8_t)(n + i), PARAM_GROUP) & 0xff) == 1) {
            afg = (uint8_t)(n + i);
        }
    }
    if (!afg) {
        kprintf("[hda] codec %u: no audio function group (nodes %x)\n", codec, root);
        return false;
    }
    command(afg, SET_POWER, 0);
    uint32_t nodes = parameter(afg, PARAM_NODES);
    unsigned first = (nodes >> 16) & 0xff, count = nodes & 0xff;
    /* A pin that can output and is connected to something; speakers and
     * headphones before the rest. */
    int best = -1, best_score = -1;
    for (unsigned i = 0; i < count; i++) {
        uint8_t node = (uint8_t)(first + i);
        if (widget_type(node) != WIDGET_PIN || !(parameter(node, PARAM_PIN) & (1u << 4))) {
            continue;
        }
        uint32_t config = command(node, GET_CONFIG, 0);
        unsigned connectivity = config >> 30, device = (config >> 20) & 0xf;
        if (connectivity == 1) { /* Nothing plugged in there. */
            continue;
        }
        int score = device == 1 ? 3 : device == 2 ? 2 : device == 0 ? 1 : 0;
        if (score > best_score) {
            best = node, best_score = score;
        }
    }
    if (best < 0 || !find_dac((uint8_t)best, 0)) {
        kprintf("[hda] codec %u: no output pin with a DAC (widgets %u-%u, pin %d)\n", codec,
                first, first + count - 1, best);
        for (unsigned i = 0; i < count; i++) {
            uint8_t node = (uint8_t)(first + i);
            kprintf("[hda]   node %u: caps %x pin %x config %x conns %x\n", node,
                    parameter(node, PARAM_WIDGET), parameter(node, PARAM_PIN),
                    command(node, GET_CONFIG, 0), parameter(node, PARAM_CONNECTIONS));
        }
        return false;
    }
    for (int i = 0; i < path_length; i++) {
        command(path[i], SET_POWER, 0);
        unmute(path[i], i + 1 < path_length ? path_input[i] : 0);
    }
    uint8_t pin = path[0], dac = path[path_length - 1];
    command(pin, SET_PIN, 0xc0); /* Output, headphone amplifier. */
    command(pin, SET_EAPD, 2);
    command(dac, SET_STREAM, STREAM_TAG << 4);
    command(dac, SET_FORMAT, format_word());
    kprintf("[hda] codec %u: DAC %u to pin %u (%d widgets on the way)\n", codec, dac, pin,
            path_length - 2);
    return true;
}

static bool setup_stream(void) {
    uint16_t caps = r16(GCAP);
    unsigned inputs = (caps >> 8) & 0xf, outputs = (caps >> 12) & 0xf;
    if (!outputs) {
        return false;
    }
    stream = regs + 0x80 + inputs * 0x20; /* The first output stream. */
    ring = dma_pages(4);
    struct bdl_entry *bdl = dma_pages(0);
    if (!ring || !bdl) {
        return false;
    }
    for (int i = 0; i < PIECES; i++) {
        bdl[i].address = virt_to_phys(ring) + (uint64_t)i * (RING_SIZE / PIECES);
        bdl[i].length = RING_SIZE / PIECES;
        bdl[i].flags = 0;
    }
    volatile uint8_t *ctl = stream + SD_CTL;
    *ctl = SD_RESET;
    uint64_t deadline = timer_ms() + 100;
    while (!(*ctl & SD_RESET) && timer_ms() < deadline) {
        thread_yield();
    }
    *ctl = 0;
    while ((*ctl & SD_RESET) && timer_ms() < deadline + 100) {
        thread_yield();
    }
    uint64_t bdl_phys = virt_to_phys(bdl);
    *(volatile uint32_t *)(stream + SD_BDPL) = (uint32_t)bdl_phys;
    *(volatile uint32_t *)(stream + SD_BDPU) = (uint32_t)(bdl_phys >> 32);
    *(volatile uint32_t *)(stream + SD_CBL) = RING_SIZE;
    *(volatile uint16_t *)(stream + SD_LVI) = PIECES - 1;
    *(volatile uint16_t *)(stream + SD_FMT) = format_word();
    return true;
}

static bool probe(struct pci_device *pci) {
    pci_enable(pci);
    regs = pci_map_bar(pci, 0);
    if (!regs) {
        return false;
    }
    /* Reset the controller, then bring it out of reset; codecs announce themselves. */
    w32(GCTL, r32(GCTL) & ~1u);
    if (!wait_for(GCTL, 1, 0, true)) {
        return false;
    }
    w32(GCTL, r32(GCTL) | 1u);
    if (!wait_for(GCTL, 1, 1, true)) {
        return false;
    }
    thread_sleep_ms(2); /* Codecs need a moment after the link comes up. */
    uint16_t present = r16(STATESTS);
    w32(INTCTL, 0); /* Polled. */
    if (!present) {
        kprintf("[hda] no codecs\n");
        return false;
    }
    while (!(present & (1u << codec))) {
        codec++;
    }
    if (!setup_rings()) {
        kprintf("[hda] the command rings didn't start\n");
        return false;
    }
    if (!setup_codec() || !setup_stream()) {
        kprintf("[hda] no way to play found\n");
        return false;
    }
    return true;
}

void hda_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->class_code == 0x04 && pci->subclass == 0x03 && probe(pci)) {
            pci_claim(pci, "hda", NULL);
            device_set_details(pci->node, "/dev/audio0, codec %u", codec);
            memcpy(output.name, "Built-in (HD Audio)", 20);
            output.rate = 48000;
            output.start = hda_start;
            thread_create("hda", ticker, NULL);
            sound_register(&output);
            return;
        }
    }
}
