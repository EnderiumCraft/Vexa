/* play: plays sound on /dev/audio0.
 *
 *   play file.wav             a WAV file (16-bit PCM, 8000 to 96000 Hz, mono or stereo)
 *   play --tone HZ [SECONDS]  a sine wave (one second unless told)
 *   play --chime              two short notes (the sound for notifications)
 *   play --welcome            the sound of the Welcome: a bell-like arpeggio up a
 *                             chord, which rings out */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>

static int audio = -1;

static int open_audio(unsigned rate, unsigned channels) {
    audio = vx_open("/dev/audio0", VX_OPEN_WRITE);
    if (audio < 0) {
        fprintf(stderr, "play: no sound device (/dev/audio0)\n");
        return -1;
    }
    struct vx_audio_format format = {rate, channels};
    if (vx_control(audio, VX_AUDIO_SET_FORMAT, &format, sizeof(format))) {
        fprintf(stderr, "play: the device can't play %u Hz, %u channels\n", rate, channels);
        return -1;
    }
    return 0;
}

static int play_all(const void *samples, size_t size) {
    const char *p = samples;
    while (size) {
        long n = vx_write(audio, p, size);
        if (n <= 0) {
            fprintf(stderr, "play: writing failed (%ld)\n", n);
            return -1;
        }
        p += n, size -= (size_t)n;
    }
    vx_control(audio, VX_AUDIO_DRAIN, NULL, 0);
    return 0;
}

static unsigned le16(const unsigned char *p) {
    return p[0] | p[1] << 8;
}

static unsigned le32(const unsigned char *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static int play_wav(const char *path) {
    int file = vx_open(path, VX_OPEN_READ);
    struct vx_stat st;
    if (file < 0 || vx_handle_stat(file, &st)) {
        fprintf(stderr, "play: can't open %s\n", path);
        return 1;
    }
    unsigned char *data = malloc(st.size ? st.size : 1);
    long got = data ? vx_read(file, data, st.size) : -1;
    vx_close(file);
    if (got < 44 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) {
        fprintf(stderr, "play: %s isn't a WAV file\n", path);
        return 1;
    }
    unsigned rate = 0, channels = 0, bits = 0, codec = 0;
    for (long at = 12; at + 8 <= got;) {
        unsigned size = le32(data + at + 4);
        if (!memcmp(data + at, "fmt ", 4) && size >= 16) {
            codec = le16(data + at + 8);
            channels = le16(data + at + 10);
            rate = le32(data + at + 12);
            bits = le16(data + at + 22);
        } else if (!memcmp(data + at, "data", 4)) {
            if (codec != 1 || bits != 16) {
                fprintf(stderr, "play: only 16-bit PCM WAV files\n");
                return 1;
            }
            size_t length = size < (unsigned long)(got - at - 8) ? size : (size_t)(got - at - 8);
            printf("play: %s: %u Hz, %u channel%s, %lu.%lu seconds\n", path, rate, channels,
                   channels == 1 ? "" : "s", length / (rate * channels * 2),
                   length * 10 / (rate * channels * 2) % 10);
            if (open_audio(rate, channels) || play_all(data + at + 8, length)) {
                return 1;
            }
            printf("play: done\n");
            return 0;
        }
        at += 8 + size + (size & 1);
    }
    fprintf(stderr, "play: no sound in %s\n", path);
    return 1;
}

/* A sine wave without a maths library: s[n+1] = 2 cos(w) s[n] - s[n-1]. */
static int play_tone(unsigned hz, unsigned seconds) {
    const unsigned rate = 48000;
    if (hz < 20 || hz > 20000 || seconds < 1 || seconds > 60) {
        fprintf(stderr, "play: a tone from 20 to 20000 Hz, for 1 to 60 seconds\n");
        return 1;
    }
    double w = 2 * 3.14159265358979 * hz / rate, w2 = w * w;
    double c = 1 - w2 / 2 + w2 * w2 / 24 - w2 * w2 * w2 / 720; /* cos(w) */
    double previous = 0, now = w - w * w2 / 6 + w * w2 * w2 / 120; /* sin(0), sin(w) */
    size_t frames = (size_t)rate * seconds;
    short *samples = malloc(frames * 4);
    if (!samples) {
        return 1;
    }
    for (size_t i = 0; i < frames; i++) {
        short value = (short)(previous * 12000); /* About a third of full scale. */
        samples[2 * i] = samples[2 * i + 1] = value;
        double next = 2 * c * now - previous;
        previous = now, now = next;
    }
    printf("play: a %u Hz tone for %u second%s\n", hz, seconds, seconds == 1 ? "" : "s");
    if (open_audio(rate, 2) || play_all(samples, frames * 4)) {
        return 1;
    }
    printf("play: done\n");
    return 0;
}

/* Two bell-like notes, each fading out: E6, then A6. */
static int play_chime(void) {
    const unsigned rate = 48000;
    static const unsigned notes[2] = {1319, 1760};
    static const float lengths[2] = {0.12f, 0.35f};
    size_t frames = (size_t)(rate * (lengths[0] + lengths[1]));
    short *samples = calloc(frames, 4);
    if (!samples) {
        return 1;
    }
    size_t at = 0;
    for (int n = 0; n < 2; n++) {
        double w = 2 * 3.14159265358979 * notes[n] / rate, w2 = w * w;
        double c = 1 - w2 / 2 + w2 * w2 / 24 - w2 * w2 * w2 / 720;
        double previous = 0, now = w - w * w2 / 6 + w * w2 * w2 / 120, level = 9000;
        size_t count = (size_t)(rate * lengths[n]);
        double fade = 1 - 6.0 / (double)count; /* (About 1/400 left at the end.) */
        for (size_t i = 0; i < count && at < frames; i++, at++) {
            short value = (short)(previous * level);
            samples[2 * at] = samples[2 * at + 1] = value;
            double next = 2 * c * now - previous;
            previous = now, now = next;
            level *= fade;
        }
    }
    if (open_audio(rate, 2) || play_all(samples, frames * 4)) {
        return 1;
    }
    return 0;
}

/* sin(x) and cos(x) from their Taylor series: close enough for the small angles
 * (under one radian) of the partials below. */
static double sine(double x) {
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}

static double cosine(double x) {
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56)));
}

/* A partial of a note: a sine from the recurrence of play_tone(). */
struct partial {
    double c, previous, now, amplitude;
};

/* A note of the welcome: its pitch, when it starts, and how long it takes to fade by 1/e. */
struct welcome_note {
    double hz, start, decay;
};

/* The sound of the Welcome: C5, E5, G5 going up, then the chord C6, E6, G6 ringing out.
 * Each note has the octaves above it as well, fainter, which makes it sound like a bell. */
static int play_welcome(void) {
    const unsigned rate = 48000;
    static const struct welcome_note notes[] = {
        {523.25, 0.00, 0.8}, {659.25, 0.13, 0.8}, {783.99, 0.26, 0.8},
        {1046.50, 0.40, 1.2}, {1318.51, 0.40, 1.2}, {1567.98, 0.40, 1.2},
    };
    static const double partials[4] = {1.0, 0.4, 0.18, 0.08};
    const double attack = 0.008, release = 0.3; /* Seconds to swell in, and to fade out at the end. */
    size_t frames = (size_t)(rate * 2.6);
    double *mix = calloc(frames, sizeof(double));
    if (!mix) {
        return 1;
    }
    for (unsigned n = 0; n < sizeof(notes) / sizeof(notes[0]); n++) {
        size_t first = (size_t)(notes[n].start * rate);
        struct partial p[4];
        for (int k = 0; k < 4; k++) {
            double w = 2 * 3.14159265358979 * notes[n].hz * (k + 1) / rate;
            p[k] = (struct partial){2 * cosine(w), 0, sine(w), partials[k]};
        }
        double level = 1, fade = 1 - 1 / (notes[n].decay * rate);
        for (size_t i = first, step = 0; i < frames && level > 0.001; i++, step++) {
            double swell = step < attack * rate ? step / (attack * rate) : 1;
            double sum = 0;
            for (int k = 0; k < 4; k++) {
                sum += p[k].amplitude * p[k].now;
                double next = p[k].c * p[k].now - p[k].previous;
                p[k].previous = p[k].now, p[k].now = next;
            }
            mix[i] += swell * level * sum;
            level *= fade;
        }
    }
    double peak = 0;
    for (size_t i = 0; i < frames; i++) {
        if (frames - i < release * rate) {
            mix[i] *= (frames - i) / (release * rate);
        }
        double size = mix[i] < 0 ? -mix[i] : mix[i];
        peak = size > peak ? size : peak;
    }
    short *samples = malloc(frames * 4);
    if (!samples) {
        free(mix);
        return 1;
    }
    for (size_t i = 0; i < frames; i++) {
        short value = (short)(mix[i] / peak * 18000); /* About 55% of full scale. */
        samples[2 * i] = samples[2 * i + 1] = value;
    }
    free(mix);
    int status = open_audio(rate, 2) || play_all(samples, frames * 4);
    free(samples);
    return status;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--chime")) {
        return play_chime();
    }
    if (argc == 2 && !strcmp(argv[1], "--welcome")) {
        return play_welcome();
    }
    if (argc >= 3 && !strcmp(argv[1], "--tone")) {
        return play_tone((unsigned)atoi(argv[2]), argc > 3 ? (unsigned)atoi(argv[3]) : 1);
    }
    if (argc == 2 && argv[1][0] != '-') {
        return play_wav(argv[1]);
    }
    fprintf(stderr, "usage: play file.wav | play --tone HZ [SECONDS] | play --chime | play --welcome\n");
    return 2;
}
