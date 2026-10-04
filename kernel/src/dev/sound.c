#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/object.h>
#include <vexa/sched.h>
#include <vexa/sound.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * The sound core: /dev/audio0, its streams, the volume, and the outputs.
 *
 * Every handle opened for writing is a stream: a ring of stereo frames at
 * the rate its program chose. The current output (the newest one: a USB
 * sound card plugged in wins over the built-in HD Audio) pulls from
 * sound_mix as it plays: each stream resampled to the output's rate (linear
 * interpolation), added up, scaled by the volume (its square: what sounds
 * even to the ear), clipped. Writers wait for room in their own ring, so a
 * program that stops writing holds up nobody else.
 *
 * With no output left (the USB card that was the only one unplugged), a
 * thread consumes the streams in real time, so programs don't wait forever.
 */

#define STREAM_FRAMES 15360 /* What a stream holds: 320 ms at 48 kHz. */
#define MIN_RATE 8000
#define MAX_RATE 96000
#define CHUNK 1024          /* sound_mix works in pieces this big. */

struct stream {
    struct vx_audio_format format;
    int16_t *frames;  /* STREAM_FRAMES stereo frames. */
    uint32_t head, count;
    uint64_t frac;    /* 32.32: how far past `head` the next output frame is. */
    struct stream *next;
};

static struct mutex lock = MUTEX_INIT;
static struct stream *streams;
static struct sound_output *outputs, *current;
static struct sound_output null_output = {.name = "None", .rate = 48000};
static unsigned next_id = 1, volume = 80, changes;
static bool muted, started;
static struct wait_queue room = WAIT_QUEUE_INIT;
static int32_t mix_buffer[CHUNK * 2];

/* ---- Mixing ---- */

static void mix_chunk(struct sound_output *output, int16_t *out, unsigned frames, bool *any,
                      bool *consumed) {
    memset(mix_buffer, 0, frames * 2 * sizeof(int32_t));
    for (struct stream *s = streams; s; s = s->next) {
        if (!s->count) {
            continue;
        }
        *any = true;
        uint64_t step = ((uint64_t)s->format.rate << 32) / output->rate;
        for (unsigned i = 0; i < frames && s->count; i++) {
            const int16_t *a = s->frames + s->head * 2;
            const int16_t *b = s->count > 1 ? s->frames + (s->head + 1) % STREAM_FRAMES * 2 : a;
            int32_t t = (int32_t)(s->frac >> 16); /* 0 to 65535 */
            mix_buffer[i * 2] += a[0] + (int32_t)(((int64_t)(b[0] - a[0]) * t) >> 16);
            mix_buffer[i * 2 + 1] += a[1] + (int32_t)(((int64_t)(b[1] - a[1]) * t) >> 16);
            s->frac += step;
            while (s->frac >> 32 && s->count) {
                s->frac -= 1ULL << 32;
                s->head = (s->head + 1) % STREAM_FRAMES;
                s->count--;
                *consumed = true;
            }
        }
    }
    int32_t gain = muted ? 0 : (int32_t)(volume * volume * 65536 / 10000);
    for (unsigned i = 0; i < frames * 2; i++) {
        int32_t v = (int32_t)(((int64_t)mix_buffer[i] * gain) >> 16);
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

bool sound_mix(struct sound_output *output, int16_t *out, unsigned frames) {
    mutex_lock(&lock);
    if (output != current) {
        mutex_unlock(&lock);
        memset(out, 0, frames * 4);
        return false;
    }
    bool any = false, consumed = false;
    while (frames) {
        unsigned n = frames < CHUNK ? frames : CHUNK;
        mix_chunk(output, out, n, &any, &consumed);
        out += n * 2;
        frames -= n;
    }
    mutex_unlock(&lock);
    if (consumed) {
        wait_queue_wake_all(&room);
    }
    return any;
}

/* With no output: the streams play into nothing, in real time. */
static void clock_thread(void *arg) {
    (void)arg;
    static int16_t scratch[480 * 2];
    for (;;) {
        thread_sleep_ms(10);
        if (current == &null_output) {
            sound_mix(&null_output, scratch, 480);
        }
    }
}

/* ---- Outputs ---- */

static const struct vnode_ops audio_ops;

void sound_register(struct sound_output *output) {
    mutex_lock(&lock);
    output->id = next_id++;
    output->next = outputs;
    outputs = output;
    current = output;
    changes++;
    bool first = !started;
    started = true;
    mutex_unlock(&lock);
    if (first) {
        devfs_add("audio0", &audio_ops, NULL);
        thread_create("sound", clock_thread, NULL);
    }
    kprintf("[sound] output %u: %s, %u Hz (now playing here)\n", output->id, output->name,
            output->rate);
}

void sound_unregister(struct sound_output *output) {
    mutex_lock(&lock);
    for (struct sound_output **link = &outputs; *link; link = &(*link)->next) {
        if (*link == output) {
            *link = output->next;
            break;
        }
    }
    if (current == output) {
        current = outputs ? outputs : &null_output;
    }
    changes++;
    mutex_unlock(&lock);
    kprintf("[sound] output %u (%s) is gone; playing on %s\n", output->id, output->name,
            current->name);
}

/* ---- /dev/audio0 ---- */

static int audio_open(struct file *file) {
    if (!(file->flags & VX_OPEN_WRITE)) {
        return 0; /* (For the volume and the outputs.) */
    }
    struct stream *s = kzalloc(sizeof(*s));
    if (s) {
        s->frames = kmalloc(STREAM_FRAMES * 4);
    }
    if (!s || !s->frames) {
        kfree(s);
        return -VX_ENOMEM;
    }
    s->format = (struct vx_audio_format){48000, 2};
    mutex_lock(&lock);
    s->next = streams;
    streams = s;
    mutex_unlock(&lock);
    file->private = s;
    return 0;
}

static bool empty(void *arg) {
    return ((struct stream *)arg)->count == 0;
}

/* Waits until what the stream has is played (as far as the output says). */
static void drain(struct stream *s) {
    uint64_t deadline = timer_ms() + 10000;
    while (!empty(s) && timer_ms() < deadline) {
        thread_sleep_ms(5);
    }
    struct sound_output *output = current;
    if (output && output->rate) {
        uint64_t ms = (uint64_t)output->queued * 1000 / output->rate;
        thread_sleep_ms(ms < 1000 ? ms : 1000);
    }
}

static void audio_close(struct file *file) {
    struct stream *s = file->private;
    if (!s) {
        return;
    }
    drain(s);
    mutex_lock(&lock);
    for (struct stream **link = &streams; *link; link = &(*link)->next) {
        if (*link == s) {
            *link = s->next;
            break;
        }
    }
    mutex_unlock(&lock);
    kfree(s->frames);
    kfree(s);
}

static bool has_room(void *arg) {
    return ((struct stream *)arg)->count < STREAM_FRAMES;
}

static int64_t audio_write(struct file *file, const void *buffer, size_t size) {
    struct stream *s = file->private;
    if (!s) {
        return -VX_EACCES;
    }
    const int16_t *in = buffer;
    unsigned channels = s->format.channels;
    size_t total = size / (channels * 2), done = 0;
    while (done < total) {
        mutex_lock(&lock);
        size_t n = STREAM_FRAMES - s->count;
        n = n < total - done ? n : total - done;
        for (size_t i = 0; i < n; i++) {
            const int16_t *frame = in + (done + i) * channels;
            int16_t *to = s->frames + (s->head + s->count + i) % STREAM_FRAMES * 2;
            to[0] = frame[0];
            to[1] = frame[channels == 2 ? 1 : 0];
        }
        s->count += (uint32_t)n;
        done += n;
        struct sound_output *output = current;
        if (n && output && output->start) {
            output->start(output);
        }
        mutex_unlock(&lock);
        if (done < total) {
            if (file->object.flags & OBJECT_NONBLOCK) {
                break;
            }
            int error = wait_queue_wait_interruptible(&room, has_room, s);
            if (error) {
                if (!done) {
                    return error;
                }
                break;
            }
        }
    }
    if (!done && total && (file->object.flags & OBJECT_NONBLOCK)) {
        return -VX_EAGAIN;
    }
    return (int64_t)(done * channels * 2);
}

static uint32_t audio_poll(struct file *file) {
    struct stream *s = file->private;
    return !s || has_room(s) ? OBJECT_WRITABLE : 0;
}

static int audio_control(struct file *file, uint32_t request, void *arg, size_t size) {
    struct stream *s = file->private;
    switch (request) {
    case VX_AUDIO_INFO: {
        if (size < sizeof(struct vx_audio_info)) {
            return -VX_EINVAL;
        }
        struct vx_audio_info *info = arg;
        memset(info, 0, sizeof(*info));
        mutex_lock(&lock);
        memcpy(info->name, current ? current->name : "None", sizeof(info->name) - 1);
        mutex_unlock(&lock);
        info->format = s ? s->format : (struct vx_audio_format){48000, 2};
        info->buffer_frames = STREAM_FRAMES;
        return 0;
    }
    case VX_AUDIO_SET_FORMAT: {
        const struct vx_audio_format *f = arg;
        if (size < sizeof(*f) || f->rate < MIN_RATE || f->rate > MAX_RATE ||
            (f->channels != 1 && f->channels != 2)) {
            return -VX_EINVAL;
        }
        if (!s) {
            return -VX_EACCES;
        }
        mutex_lock(&lock);
        int error = s->count ? -VX_EBUSY : 0;
        if (!error) {
            s->format = *f;
            s->frac = 0;
        }
        mutex_unlock(&lock);
        return error;
    }
    case VX_AUDIO_DELAY: {
        if (size < sizeof(unsigned) || !s) {
            return s ? -VX_EINVAL : -VX_EACCES;
        }
        mutex_lock(&lock);
        uint64_t frames = s->count;
        if (current && current->rate) {
            frames += (uint64_t)current->queued * s->format.rate / current->rate;
        }
        mutex_unlock(&lock);
        *(unsigned *)arg = (unsigned)frames;
        return 0;
    }
    case VX_AUDIO_DRAIN:
        if (s) {
            drain(s);
        }
        return 0;
    case VX_AUDIO_DROP:
        if (s) {
            mutex_lock(&lock);
            s->count = 0;
            s->frac = 0;
            mutex_unlock(&lock);
            wait_queue_wake_all(&room);
        }
        return 0;
    case VX_AUDIO_GET_VOLUME:
    case VX_AUDIO_SET_VOLUME: {
        struct vx_audio_volume *v = arg;
        if (size < sizeof(*v)) {
            return -VX_EINVAL;
        }
        mutex_lock(&lock);
        if (request == VX_AUDIO_SET_VOLUME) {
            volume = v->volume > 100 ? 100 : v->volume;
            muted = v->muted != 0;
            changes++;
        }
        v->volume = volume;
        v->muted = muted;
        v->changes = changes;
        mutex_unlock(&lock);
        return 0;
    }
    case VX_AUDIO_OUTPUTS: {
        struct vx_audio_outputs *o = arg;
        if (size < sizeof(*o)) {
            return -VX_EINVAL;
        }
        memset(o, 0, sizeof(*o));
        mutex_lock(&lock);
        /* (Oldest first: the order they came in.) */
        struct sound_output *list[VX_AUDIO_MAX_OUTPUTS];
        unsigned n = 0;
        for (struct sound_output *out = outputs; out && n < VX_AUDIO_MAX_OUTPUTS; out = out->next) {
            list[n++] = out;
        }
        for (unsigned i = 0; i < n; i++) {
            struct sound_output *out = list[n - 1 - i];
            o->output[i].id = out->id;
            o->output[i].rate = out->rate;
            memcpy(o->output[i].name, out->name, sizeof(o->output[i].name) - 1);
        }
        o->count = n;
        o->current = current && current != &null_output ? current->id : 0;
        mutex_unlock(&lock);
        return 0;
    }
    case VX_AUDIO_SET_OUTPUT: {
        if (size < sizeof(unsigned)) {
            return -VX_EINVAL;
        }
        unsigned id = *(unsigned *)arg;
        int error = -VX_ENOENT;
        mutex_lock(&lock);
        for (struct sound_output *out = outputs; out; out = out->next) {
            if (out->id == id) {
                current = out;
                changes++;
                error = 0;
                if (out->start) {
                    out->start(out);
                }
            }
        }
        mutex_unlock(&lock);
        if (!error) {
            kprintf("[sound] playing on %s\n", current->name);
        }
        return error;
    }
    default:
        return -VX_ENOTTY;
    }
}

static const struct vnode_ops audio_ops = {
    .open = audio_open,
    .close = audio_close,
    .file_write = audio_write,
    .file_poll = audio_poll,
    .control = audio_control,
};
