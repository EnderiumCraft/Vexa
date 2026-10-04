/* play: plays sound on /dev/audio0.
 *
 *   play file.wav             a WAV file (16-bit PCM, 8000 to 96000 Hz, mono or stereo)
 *   play --tone HZ [SECONDS]  a sine wave (one second unless told)
 *   play --chime              two short notes (the sound for notifications) */
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

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--chime")) {
        return play_chime();
    }
    if (argc >= 3 && !strcmp(argv[1], "--tone")) {
        return play_tone((unsigned)atoi(argv[2]), argc > 3 ? (unsigned)atoi(argv[3]) : 1);
    }
    if (argc == 2 && argv[1][0] != '-') {
        return play_wav(argv[1]);
    }
    fprintf(stderr, "usage: play file.wav | play --tone HZ [SECONDS] | play --chime\n");
    return 2;
}
