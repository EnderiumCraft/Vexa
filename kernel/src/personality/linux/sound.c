/*
 * ALSA for Linux programs: the kernel interface that alsa-lib (and so aplay,
 * SDL, Firefox...) uses, over Vexa's /dev/audio0. One card with one playback
 * device: /dev/snd/controlC0 (what the card is) and /dev/snd/pcmC0D0p (the
 * sound). Interleaved 16-bit samples written with WRITEI_FRAMES, at 44100 or
 * 48000 Hz, mono or stereo; alsa-lib's "plug" converts anything else. The
 * status and control pages aren't mapped, so alsa-lib uses SYNC_PTR instead.
 * Running out of samples isn't an xrun: the device plays silence meanwhile.
 */
#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/fs.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/object.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>
#include <vexa/vfs.h>
#include "linux.h"

#define IOC_NR(r) ((r)&0xff)
#define IOC_TYPE(r) (((r) >> 8) & 0xff)
#define IOC_SIZE(r) (((r) >> 16) & 0x3fff)

/* ---- The structures (uapi/sound/asound.h) ---- */

struct snd_ctl_card_info {
    int card, pad;
    unsigned char id[16], driver[16], name[32], longname[80], reserved_[16], mixername[80],
        components[128];
};

struct snd_ctl_elem_list {
    unsigned offset, space, used, count;
    uint64_t pids;
    unsigned char reserved[50];
};

struct snd_pcm_info {
    unsigned device, subdevice;
    int stream, card;
    unsigned char id[64], name[80], subname[32];
    int dev_class, dev_subclass;
    unsigned subdevices_count, subdevices_avail;
    unsigned char sync[16];
    unsigned char reserved[64];
};

struct snd_mask {
    uint32_t bits[8];
};

struct snd_interval {
    unsigned min, max;
    unsigned flags; /* openmin:1, openmax:1, integer:1, empty:1 */
};
#define OPENMIN 1u
#define OPENMAX 2u
#define INTEGER 4u
#define EMPTY 8u

struct snd_pcm_hw_params {
    unsigned flags;
    struct snd_mask masks[3];
    struct snd_mask mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    unsigned rmask, cmask, info, msbits, rate_num, rate_den;
    uint64_t fifo_size;
    unsigned char reserved[64];
};

struct snd_pcm_sw_params {
    int tstamp_mode;
    unsigned period_step, sleep_min;
    uint64_t avail_min, xfer_align, start_threshold, stop_threshold, silence_threshold,
        silence_size, boundary;
    unsigned proto, tstamp_type;
    unsigned char reserved[56];
};

struct linux_timespec64 {
    int64_t sec, nsec;
};

struct snd_pcm_status {
    int state, pad0;
    struct linux_timespec64 trigger_tstamp, tstamp;
    uint64_t appl_ptr, hw_ptr;
    int64_t delay;
    uint64_t avail, avail_max, overrange;
    int suspended_state;
    unsigned audio_tstamp_data;
    struct linux_timespec64 audio_tstamp, driver_tstamp;
    unsigned audio_tstamp_accuracy;
    unsigned char reserved[52 - 2 * sizeof(struct linux_timespec64)];
};

struct snd_pcm_mmap_status {
    int state, pad1;
    uint64_t hw_ptr;
    struct linux_timespec64 tstamp;
    int suspended_state, pad2;
    struct linux_timespec64 audio_tstamp;
};

struct snd_pcm_mmap_control {
    uint64_t appl_ptr, avail_min;
};

struct snd_pcm_sync_ptr {
    unsigned flags, pad;
    union {
        struct snd_pcm_mmap_status status;
        unsigned char reserved[64];
    } s;
    union {
        struct snd_pcm_mmap_control control;
        unsigned char reserved[64];
    } c;
};

struct snd_xferi {
    int64_t result;
    uint64_t buf;
    uint64_t frames;
};

enum {
    STATE_OPEN, STATE_SETUP, STATE_PREPARED, STATE_RUNNING, STATE_XRUN, STATE_DRAINING,
    STATE_PAUSED,
};

enum { /* hw_params: masks, then intervals */
    ACCESS, FORMAT, SUBFORMAT,
    SAMPLE_BITS = 8, FRAME_BITS, CHANNELS, RATE, PERIOD_TIME, PERIOD_SIZE, PERIOD_BYTES,
    PERIODS, BUFFER_TIME, BUFFER_SIZE, BUFFER_BYTES, TICK_TIME,
};

#define ACCESS_RW_INTERLEAVED 3
#define FORMAT_S16_LE 2
#define SUBFORMAT_STD 0
#define SYNC_PTR_APPL 2
#define SYNC_PTR_AVAIL_MIN 4
#define INFO_INTERLEAVED 0x100
#define INFO_BLOCK_TRANSFER 0x10000

#define MAX_FRAMES 15360 /* What /dev/audio0 holds. */

/* ---- hw_params refinement ----
 * Each parameter is narrowed to what the device can do, then the rules that
 * tie them together are applied until nothing changes. */

static struct snd_interval *iv(struct snd_pcm_hw_params *p, int which) {
    return &p->intervals[which - SAMPLE_BITS];
}

static bool empty(const struct snd_interval *i) {
    return (i->flags & EMPTY) || i->min > i->max;
}

/* Narrows to [min, max]; returns true if anything changed. */
static bool narrow(struct snd_interval *i, uint64_t min, uint64_t max) {
    bool changed = false;
    if (max > 0xffffffffu) {
        max = 0xffffffffu;
    }
    if (i->min < min || ((i->flags & OPENMIN) && i->min <= min)) {
        i->min = (unsigned)min, changed = true;
        i->flags &= ~OPENMIN;
    }
    if (i->max > max || ((i->flags & OPENMAX) && i->max >= max)) {
        i->max = (unsigned)max, changed = true;
        i->flags &= ~OPENMAX;
    }
    if (i->min > i->max) {
        i->flags |= EMPTY;
    }
    return changed;
}

/* Open ends become closed: these are all whole numbers. */
static void close_ends(struct snd_interval *i) {
    if ((i->flags & OPENMIN) && i->min < 0xffffffffu) {
        i->min++;
    }
    if ((i->flags & OPENMAX) && i->max > 0) {
        i->max--;
    }
    i->flags = (i->flags & EMPTY) | INTEGER;
    if (i->min > i->max) {
        i->flags |= EMPTY;
    }
}

static uint64_t div_up(uint64_t a, uint64_t b) {
    return b ? (a + b - 1) / b : 0xffffffffu;
}

static int refine(struct snd_pcm_hw_params *p) {
    p->masks[ACCESS].bits[0] &= 1u << ACCESS_RW_INTERLEAVED;
    p->masks[FORMAT].bits[0] &= 1u << FORMAT_S16_LE;
    p->masks[SUBFORMAT].bits[0] &= 1u << SUBFORMAT_STD;
    for (int m = 0; m < 3; m++) {
        for (int w = 1; w < 8; w++) {
            p->masks[m].bits[w] = 0;
        }
        if (!p->masks[m].bits[0]) {
            return -LE_EINVAL;
        }
    }
    for (int i = SAMPLE_BITS; i <= TICK_TIME; i++) {
        close_ends(iv(p, i));
    }
    narrow(iv(p, SAMPLE_BITS), 16, 16);
    narrow(iv(p, CHANNELS), 1, 2);
    narrow(iv(p, PERIOD_SIZE), 64, MAX_FRAMES / 2);
    narrow(iv(p, PERIODS), 2, 64);
    narrow(iv(p, BUFFER_SIZE), 128, MAX_FRAMES);
    narrow(iv(p, TICK_TIME), 0, 0);
    /* Rates: 44100 and 48000 only. */
    struct snd_interval *rate = iv(p, RATE);
    bool r44 = rate->min <= 44100 && rate->max >= 44100;
    bool r48 = rate->min <= 48000 && rate->max >= 48000;
    if (!r44 && !r48) {
        return -LE_EINVAL;
    }
    rate->min = r44 ? 44100 : 48000;
    rate->max = r48 ? 48000 : 44100;
    for (int pass = 0; pass < 16; pass++) {
        bool changed = false;
        struct snd_interval *ch = iv(p, CHANNELS), *fb = iv(p, FRAME_BITS);
        struct snd_interval *ps = iv(p, PERIOD_SIZE), *pb = iv(p, PERIOD_BYTES);
        struct snd_interval *n = iv(p, PERIODS), *bs = iv(p, BUFFER_SIZE);
        struct snd_interval *bb = iv(p, BUFFER_BYTES), *pt = iv(p, PERIOD_TIME);
        struct snd_interval *bt = iv(p, BUFFER_TIME);
        /* frame bits = 16 x channels */
        changed |= narrow(fb, 16ull * ch->min, 16ull * ch->max);
        changed |= narrow(ch, div_up(fb->min, 16), fb->max / 16);
        /* bytes = frames x frame bits / 8 */
        changed |= narrow(pb, (uint64_t)ps->min * fb->min / 8, (uint64_t)ps->max * fb->max / 8);
        changed |= narrow(ps, div_up(pb->min * 8ull, fb->max), pb->max * 8ull / (fb->min ? fb->min : 1));
        changed |= narrow(bb, (uint64_t)bs->min * fb->min / 8, (uint64_t)bs->max * fb->max / 8);
        changed |= narrow(bs, div_up(bb->min * 8ull, fb->max), bb->max * 8ull / (fb->min ? fb->min : 1));
        /* buffer = period x periods */
        changed |= narrow(bs, (uint64_t)ps->min * n->min, (uint64_t)ps->max * n->max);
        changed |= narrow(ps, div_up(bs->min, n->max), bs->max / (n->min ? n->min : 1));
        changed |= narrow(n, div_up(bs->min, ps->max), bs->max / (ps->min ? ps->min : 1));
        /* time (us) = frames x 1000000 / rate */
        changed |= narrow(pt, (uint64_t)ps->min * 1000000 / rate->max,
                          div_up((uint64_t)ps->max * 1000000, rate->min));
        changed |= narrow(ps, (uint64_t)pt->min * rate->min / 1000000,
                          div_up((uint64_t)pt->max * rate->max, 1000000));
        changed |= narrow(bt, (uint64_t)bs->min * 1000000 / rate->max,
                          div_up((uint64_t)bs->max * 1000000, rate->min));
        changed |= narrow(bs, (uint64_t)bt->min * rate->min / 1000000,
                          div_up((uint64_t)bt->max * rate->max, 1000000));
        for (int i = SAMPLE_BITS; i <= TICK_TIME; i++) {
            if (empty(iv(p, i))) {
                return -LE_EINVAL;
            }
        }
        if (!changed) {
            break;
        }
    }
    p->cmask = ~0u;
    p->info = INFO_INTERLEAVED | INFO_BLOCK_TRANSFER;
    p->msbits = 16;
    if (rate->min == rate->max) {
        p->rate_num = rate->min, p->rate_den = 1;
    }
    p->fifo_size = 0;
    return 0;
}

/* ---- One open playback device ---- */

struct pcm {
    struct file *audio; /* /dev/audio0, opened at HW_PARAMS. */
    int state;
    unsigned rate, channels;
    uint64_t buffer_size, period_size, avail_min, boundary;
    uint64_t appl; /* Frames written since PREPARE. */
};

static uint64_t hw_position(struct pcm *pcm) {
    unsigned delay = 0;
    if (pcm->audio) {
        vfs_control(pcm->audio, VX_AUDIO_DELAY, &delay, sizeof(delay));
    }
    return pcm->appl > delay ? pcm->appl - delay : 0;
}

static void now(struct linux_timespec64 *t) {
    uint64_t ms = timer_ms();
    t->sec = (int64_t)(ms / 1000);
    t->nsec = (int64_t)(ms % 1000) * 1000000;
}

static int64_t pcm_ioctl(struct file *file, uint32_t request, uint64_t arg) {
    struct pcm *pcm = file->private;
    unsigned size = IOC_SIZE(request);
    if (IOC_TYPE(request) != 'A') {
        return -LE_ENOTTY;
    }
    switch (IOC_NR(request)) {
    case 0x00: { /* PVERSION */
        int version = 0x2000f; /* 2.0.15 */
        return copy_to_user(arg, &version, sizeof(version)) ? 0 : -LE_EFAULT;
    }
    case 0x01: { /* INFO */
        struct snd_pcm_info info;
        memset(&info, 0, sizeof(info));
        memcpy(info.id, "HDA", 4);
        memcpy(info.name, "HD Audio", 9);
        memcpy(info.subname, "subdevice #0", 13);
        info.subdevices_count = 1;
        return copy_to_user(arg, &info, sizeof(info)) ? 0 : -LE_EFAULT;
    }
    case 0x02: /* TSTAMP */
    case 0x03: /* TTSTAMP */
    case 0x04: /* USER_PVERSION */
    case 0x22: /* HWSYNC */
    case 0x41: /* RESET */
    case 0x60: /* LINK */
    case 0x61: /* UNLINK */
        return 0;
    case 0x10:   /* HW_REFINE */
    case 0x11: { /* HW_PARAMS */
        struct snd_pcm_hw_params *p = kmalloc(sizeof(*p));
        if (!p) {
            return -LE_ENOMEM;
        }
        if (size < sizeof(*p) || !copy_from_user(p, arg, sizeof(*p))) {
            kfree(p);
            return -LE_EFAULT;
        }
        int64_t error = refine(p);
        if (!error && IOC_NR(request) == 0x11) {
            /* The choice: the smallest of each, as a single value. */
            struct snd_interval *rate = iv(p, RATE), *ch = iv(p, CHANNELS);
            struct snd_interval *ps = iv(p, PERIOD_SIZE), *bs = iv(p, BUFFER_SIZE);
            if (rate->min != rate->max || ch->min != ch->max || ps->min != ps->max ||
                bs->min != bs->max) {
                error = -LE_EINVAL;
            } else {
                if (!pcm->audio) {
                    error = linux_errno(vfs_open("/dev/audio0", 11, VX_OPEN_WRITE, &pcm->audio));
                }
                struct vx_audio_format format = {rate->min, ch->min};
                if (!error) {
                    vfs_control(pcm->audio, VX_AUDIO_DROP, NULL, 0);
                    error = linux_errno(vfs_control(pcm->audio, VX_AUDIO_SET_FORMAT, &format,
                                                    sizeof(format)));
                }
                if (!error) {
                    pcm->rate = rate->min, pcm->channels = ch->min;
                    pcm->period_size = ps->min, pcm->buffer_size = bs->min;
                    pcm->avail_min = pcm->period_size;
                    pcm->boundary = pcm->buffer_size;
                    while (pcm->boundary * 2 <= 0x7fffffffffffffffull - pcm->buffer_size) {
                        pcm->boundary *= 2;
                    }
                    pcm->state = STATE_PREPARED;
                    pcm->appl = 0;
                }
            }
        }
        if (!error && !copy_to_user(arg, p, sizeof(*p))) {
            error = -LE_EFAULT;
        }
        kfree(p);
        return error;
    }
    case 0x12: /* HW_FREE */
        if (pcm->audio) {
            vfs_control(pcm->audio, VX_AUDIO_DROP, NULL, 0);
        }
        pcm->state = STATE_OPEN;
        return 0;
    case 0x13: { /* SW_PARAMS */
        struct snd_pcm_sw_params sw;
        if (size < sizeof(sw) || !copy_from_user(&sw, arg, sizeof(sw))) {
            return -LE_EFAULT;
        }
        if (pcm->state == STATE_OPEN) {
            return -LE_EBADFD;
        }
        pcm->avail_min = sw.avail_min ? sw.avail_min : 1;
        if (sw.boundary) {
            pcm->boundary = sw.boundary;
        }
        sw.boundary = pcm->boundary;
        return copy_to_user(arg, &sw, sizeof(sw)) ? 0 : -LE_EFAULT;
    }
    case 0x20:   /* STATUS */
    case 0x24: { /* STATUS_EXT */
        struct snd_pcm_status st;
        memset(&st, 0, sizeof(st));
        uint64_t hw = hw_position(pcm);
        st.state = pcm->state;
        now(&st.tstamp);
        st.appl_ptr = pcm->appl % pcm->boundary;
        st.hw_ptr = hw % pcm->boundary;
        st.delay = (int64_t)(pcm->appl - hw);
        st.avail = pcm->buffer_size - (pcm->appl - hw);
        st.avail_max = st.avail;
        return copy_to_user(arg, &st, sizeof(st)) ? 0 : -LE_EFAULT;
    }
    case 0x21: { /* DELAY */
        int64_t delay = (int64_t)(pcm->appl - hw_position(pcm));
        return copy_to_user(arg, &delay, sizeof(delay)) ? 0 : -LE_EFAULT;
    }
    case 0x23: { /* SYNC_PTR */
        struct snd_pcm_sync_ptr sp;
        if (size < sizeof(sp) || !copy_from_user(&sp, arg, sizeof(sp))) {
            return -LE_EFAULT;
        }
        if (!(sp.flags & SYNC_PTR_AVAIL_MIN) && sp.c.control.avail_min) {
            pcm->avail_min = sp.c.control.avail_min;
        }
        /* (Without APPL, the program says where it got to; but only
         * WRITEI moves our pointer, so it can't have got further.) */
        uint64_t hw = hw_position(pcm);
        memset(&sp.s, 0, sizeof(sp.s));
        sp.s.status.state = pcm->state;
        sp.s.status.hw_ptr = hw % (pcm->boundary ? pcm->boundary : 1);
        now(&sp.s.status.tstamp);
        sp.c.control.appl_ptr = pcm->appl % (pcm->boundary ? pcm->boundary : 1);
        sp.c.control.avail_min = pcm->avail_min;
        return copy_to_user(arg, &sp, sizeof(sp)) ? 0 : -LE_EFAULT;
    }
    case 0x32: /* CHANNEL_INFO: only for mmap */
        return -LE_EINVAL;
    case 0x40: /* PREPARE */
        if (pcm->state == STATE_OPEN) {
            return -LE_EBADFD;
        }
        if (pcm->state != STATE_PREPARED) {
            vfs_control(pcm->audio, VX_AUDIO_DROP, NULL, 0);
            pcm->appl = 0;
        }
        pcm->state = STATE_PREPARED;
        return 0;
    case 0x42: /* START */
        if (pcm->state != STATE_PREPARED) {
            return -LE_EBADFD;
        }
        pcm->state = STATE_RUNNING;
        return 0;
    case 0x43: /* DROP */
        if (pcm->audio) {
            vfs_control(pcm->audio, VX_AUDIO_DROP, NULL, 0);
        }
        if (pcm->state != STATE_OPEN) {
            pcm->state = STATE_SETUP;
        }
        pcm->appl = 0;
        return 0;
    case 0x44: /* DRAIN */
        if (pcm->state == STATE_OPEN) {
            return -LE_EBADFD;
        }
        if (pcm->audio) {
            vfs_control(pcm->audio, VX_AUDIO_DRAIN, NULL, 0);
        }
        pcm->state = STATE_SETUP;
        pcm->appl = 0;
        return 0;
    case 0x45: /* PAUSE */
    case 0x47: /* RESUME */
        return -LE_ENOSYS;
    case 0x46:   /* REWIND */
    case 0x49: { /* FORWARD */
        uint64_t zero = 0;
        return copy_to_user(arg, &zero, sizeof(zero)) ? 0 : -LE_EFAULT;
    }
    case 0x50: { /* WRITEI_FRAMES */
        struct snd_xferi x;
        if (!copy_from_user(&x, arg, sizeof(x))) {
            return -LE_EFAULT;
        }
        if (pcm->state != STATE_PREPARED && pcm->state != STATE_RUNNING) {
            return pcm->state == STATE_OPEN || pcm->state == STATE_SETUP ? -LE_EBADFD
                                                                          : -LE_EPIPE;
        }
        size_t frame = pcm->channels * 2, done = 0, total = x.frames * frame;
        uint8_t *chunk = kmalloc(4096);
        if (!chunk) {
            return -LE_ENOMEM;
        }
        int64_t result = 0;
        while (done < total) {
            size_t n = total - done < 4096 ? total - done : 4096;
            n -= n % frame;
            if (!copy_from_user(chunk, x.buf + done, n)) {
                result = -LE_EFAULT;
                break;
            }
            int64_t wrote = vfs_write(pcm->audio, chunk, n);
            if (wrote <= 0) {
                result = wrote ? linux_errno(wrote) : 0;
                break;
            }
            done += (size_t)wrote;
            if ((size_t)wrote < n) {
                break;
            }
        }
        kfree(chunk);
        if (done) {
            pcm->state = STATE_RUNNING;
            pcm->appl += done / frame;
            x.result = (int64_t)(done / frame);
            return copy_to_user(arg, &x, sizeof(x)) ? 0 : -LE_EFAULT;
        }
        return result ? result : -LE_EAGAIN;
    }
    default:
        return -LE_ENOTTY;
    }
}

static int pcm_open(struct file *file) {
    struct pcm *pcm = kzalloc(sizeof(*pcm));
    if (!pcm) {
        return -VX_ENOMEM;
    }
    pcm->boundary = 1;
    file->private = pcm;
    return 0;
}

static void pcm_close(struct file *file) {
    struct pcm *pcm = file->private;
    if (pcm->audio) {
        vfs_control(pcm->audio, VX_AUDIO_DROP, NULL, 0);
        vfs_close(pcm->audio);
    }
    kfree(pcm);
}

static uint32_t pcm_poll(struct file *file) {
    struct pcm *pcm = file->private;
    if (!pcm->audio) {
        return OBJECT_WRITABLE;
    }
    uint64_t avail = pcm->buffer_size - (pcm->appl - hw_position(pcm));
    return avail >= pcm->avail_min ? OBJECT_WRITABLE : 0;
}

static const struct vnode_ops pcm_ops = {
    .open = pcm_open,
    .close = pcm_close,
    .file_poll = pcm_poll,
};

/* ---- The card ---- */

static int64_t control_ioctl(uint32_t request, uint64_t arg) {
    if (IOC_TYPE(request) != 'U') {
        return -LE_ENOTTY;
    }
    switch (IOC_NR(request)) {
    case 0x00: { /* PVERSION */
        int version = 0x20008;
        return copy_to_user(arg, &version, sizeof(version)) ? 0 : -LE_EFAULT;
    }
    case 0x01: { /* CARD_INFO */
        struct snd_ctl_card_info info;
        memset(&info, 0, sizeof(info));
        memcpy(info.id, "HDA", 4);
        memcpy(info.driver, "Vexa", 5);
        memcpy(info.name, "HD Audio", 9);
        memcpy(info.longname, "Intel HD Audio (Vexa)", 22);
        memcpy(info.mixername, "Vexa", 5);
        return copy_to_user(arg, &info, sizeof(info)) ? 0 : -LE_EFAULT;
    }
    case 0x10: { /* ELEM_LIST: no mixer controls */
        struct snd_ctl_elem_list list;
        if (!copy_from_user(&list, arg, sizeof(list))) {
            return -LE_EFAULT;
        }
        list.used = list.count = 0;
        return copy_to_user(arg, &list, sizeof(list)) ? 0 : -LE_EFAULT;
    }
    case 0x16: /* SUBSCRIBE_EVENTS */
    case 0x32: /* PCM_PREFER_SUBDEVICE */
    case 0xd0: /* POWER */
        return 0;
    case 0xd1: { /* POWER_STATE */
        int state = 0;
        return copy_to_user(arg, &state, sizeof(state)) ? 0 : -LE_EFAULT;
    }
    case 0x30: { /* PCM_NEXT_DEVICE */
        int device;
        if (!copy_from_user(&device, arg, sizeof(device))) {
            return -LE_EFAULT;
        }
        device = device < 0 ? 0 : -1;
        return copy_to_user(arg, &device, sizeof(device)) ? 0 : -LE_EFAULT;
    }
    case 0x31: { /* PCM_INFO */
        struct snd_pcm_info info;
        if (!copy_from_user(&info, arg, sizeof(info))) {
            return -LE_EFAULT;
        }
        if (info.device != 0 || info.stream != 0) {
            return -LE_ENOENT; /* Only playback, on device 0. */
        }
        memset(info.id, 0, sizeof(info.id) + sizeof(info.name) + sizeof(info.subname));
        memcpy(info.id, "HDA", 4);
        memcpy(info.name, "HD Audio", 9);
        memcpy(info.subname, "subdevice #0", 13);
        info.subdevices_count = info.subdevices_avail = 1;
        return copy_to_user(arg, &info, sizeof(info)) ? 0 : -LE_EFAULT;
    }
    default:
        return -LE_ENOTTY;
    }
}

static const struct vnode_ops control_ops = {0};

void linux_sound_init(void) {
    struct file *audio;
    if (vfs_open("/dev/audio0", 11, VX_OPEN_READ, &audio)) {
        return; /* No sound card. */
    }
    vfs_close(audio);
    devfs_add("snd/controlC0", &control_ops, NULL);
    devfs_add("snd/pcmC0D0p", &pcm_ops, NULL);
}

bool linux_sound_ioctl(struct file *file, uint32_t request, uint64_t arg, int64_t *result) {
    if (file->vnode->ops == &control_ops) {
        *result = control_ioctl(request, arg);
        return true;
    }
    if (file->vnode->ops == &pcm_ops) {
        *result = pcm_ioctl(file, request, arg);
        return true;
    }
    return false;
}
