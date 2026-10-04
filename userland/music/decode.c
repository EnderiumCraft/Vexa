/* Decoding: MP3 (minimp3), Ogg Vorbis (stb_vorbis), FLAC (dr_flac) and WAV,
 * all from a file read into memory, to 16-bit samples; and the tags (title,
 * artist, album) from ID3v2 and Vorbis comments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decode.h"

/* (The decoders themselves are compiled in codec_*.c.) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define MINIMP3_NO_STDIO
#include "../../third_party/media/minimp3_ex.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "../../third_party/media/stb_vorbis.c"
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_SIMD
#include "../../third_party/media/dr_flac.h"
#pragma GCC diagnostic pop

enum kind { K_WAV, K_MP3, K_VORBIS, K_FLAC };

struct decoder {
    enum kind kind;
    unsigned char *data;
    size_t size;
    /* WAV */
    const unsigned char *pcm;
    size_t pcm_frames, position;
    unsigned bits;
    /* The others' */
    mp3dec_ex_t mp3;
    stb_vorbis *vorbis;
    drflac *flac;
    struct decoder_info *info; /* (While opening: where FLAC's tags go.) */
};

static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = length > 0 ? malloc((size_t)length) : NULL;
    if (data && fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)length : 0;
    return data;
}

/* ---- Tags ---- */

static void copy_tag(char *out, size_t size, const char *text, size_t length) {
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\0')) {
        length--;
    }
    if (length >= size) {
        length = size - 1;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

/* An ID3v2 text frame's text (Latin-1, UTF-16 or UTF-8) as UTF-8. */
static void id3_text(char *out, size_t size, const unsigned char *p, size_t length) {
    if (!length) {
        return;
    }
    unsigned encoding = p[0];
    p++, length--;
    size_t n = 0;
    if (encoding == 1 || encoding == 2) { /* UTF-16, with or without a BOM. */
        bool big = encoding == 2;
        if (length >= 2 && p[0] == 0xfe && p[1] == 0xff) {
            big = true, p += 2, length -= 2;
        } else if (length >= 2 && p[0] == 0xff && p[1] == 0xfe) {
            big = false, p += 2, length -= 2;
        }
        for (size_t i = 0; i + 1 < length && n + 4 < size; i += 2) {
            unsigned c = big ? (unsigned)(p[i] << 8 | p[i + 1]) : (unsigned)(p[i] | p[i + 1] << 8);
            if (!c) {
                break;
            }
            if (c < 0x80) {
                out[n++] = (char)c;
            } else if (c < 0x800) {
                out[n++] = (char)(0xc0 | c >> 6), out[n++] = (char)(0x80 | (c & 0x3f));
            } else {
                out[n++] = (char)(0xe0 | c >> 12), out[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
                out[n++] = (char)(0x80 | (c & 0x3f));
            }
        }
        out[n] = '\0';
    } else if (encoding == 3) {
        copy_tag(out, size, (const char *)p, length);
    } else { /* Latin-1 */
        for (size_t i = 0; i < length && p[i] && n + 3 < size; i++) {
            if (p[i] < 0x80) {
                out[n++] = (char)p[i];
            } else {
                out[n++] = (char)(0xc0 | p[i] >> 6), out[n++] = (char)(0x80 | (p[i] & 0x3f));
            }
        }
        out[n] = '\0';
    }
}

static void id3v2(const unsigned char *d, size_t size, struct decoder_info *info) {
    if (size < 10 || memcmp(d, "ID3", 3) != 0 || d[3] < 3) {
        return;
    }
    size_t total = (size_t)(d[6] & 0x7f) << 21 | (size_t)(d[7] & 0x7f) << 14 |
                   (size_t)(d[8] & 0x7f) << 7 | (d[9] & 0x7f);
    size_t end = 10 + total < size ? 10 + total : size;
    for (size_t at = 10; at + 10 <= end;) {
        const unsigned char *f = d + at;
        if (!f[0]) {
            break;
        }
        size_t length = d[3] == 4 ? (size_t)(f[4] & 0x7f) << 21 | (size_t)(f[5] & 0x7f) << 14 |
                                        (size_t)(f[6] & 0x7f) << 7 | (f[7] & 0x7f)
                                  : (size_t)f[4] << 24 | (size_t)f[5] << 16 | (size_t)f[6] << 8 | f[7];
        if (at + 10 + length > end) {
            break;
        }
        if (!memcmp(f, "TIT2", 4)) {
            id3_text(info->title, sizeof(info->title), f + 10, length);
        } else if (!memcmp(f, "TPE1", 4)) {
            id3_text(info->artist, sizeof(info->artist), f + 10, length);
        } else if (!memcmp(f, "TALB", 4)) {
            id3_text(info->album, sizeof(info->album), f + 10, length);
        }
        at += 10 + length;
    }
}

/* "TITLE=...": a Vorbis comment (Ogg and FLAC). */
static void vorbis_comment(const char *c, size_t length, struct decoder_info *info) {
    static const struct {
        const char *key;
        size_t offset;
    } keys[] = {{"TITLE=", offsetof(struct decoder_info, title)},
                {"ARTIST=", offsetof(struct decoder_info, artist)},
                {"ALBUM=", offsetof(struct decoder_info, album)}};
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        size_t n = strlen(keys[k].key);
        if (length > n && !strncasecmp(c, keys[k].key, n)) {
            copy_tag((char *)info + keys[k].offset, sizeof(info->title), c + n, length - n);
        }
    }
}

static void flac_metadata(void *user, drflac_metadata *m) {
    struct decoder *d = user;
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT || !d->info) {
        return;
    }
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount,
                                        m->data.vorbis_comment.pComments);
    drflac_uint32 length;
    const char *c;
    while ((c = drflac_next_vorbis_comment(&it, &length)) != NULL) {
        vorbis_comment(c, length, d->info);
    }
}

/* ---- WAV ---- */

static bool open_wav(struct decoder *d, struct decoder_info *info) {
    const unsigned char *p = d->data;
    if (d->size < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
        return false;
    }
    unsigned format = 0;
    for (size_t at = 12; at + 8 <= d->size;) {
        size_t length = p[at + 4] | p[at + 5] << 8 | p[at + 6] << 16 | (size_t)p[at + 7] << 24;
        const unsigned char *c = p + at + 8;
        if (!memcmp(p + at, "fmt ", 4) && length >= 16) {
            format = c[0] | c[1] << 8;
            info->channels = c[2] | c[3] << 8;
            info->rate = c[4] | c[5] << 8 | c[6] << 16 | (unsigned)c[7] << 24;
            d->bits = c[14] | c[15] << 8;
        } else if (!memcmp(p + at, "data", 4)) {
            if (length > d->size - at - 8) {
                length = d->size - at - 8;
            }
            d->pcm = c;
            if (!info->channels || (d->bits != 8 && d->bits != 16) || format != 1) {
                return false;
            }
            d->pcm_frames = length / (info->channels * d->bits / 8);
            return true;
        }
        at += 8 + length + (length & 1);
    }
    return false;
}

static int read_wav(struct decoder *d, int16_t *out, int frames, unsigned channels) {
    int n = 0;
    while (n < frames && d->position < d->pcm_frames) {
        for (unsigned c = 0; c < channels; c++) {
            size_t i = d->position * channels + c;
            out[n * channels + c] = d->bits == 16 ? (int16_t)(d->pcm[i * 2] | d->pcm[i * 2 + 1] << 8)
                                                  : (int16_t)((d->pcm[i] - 128) << 8);
        }
        n++, d->position++;
    }
    return n;
}

/* ---- The decoder ---- */

static bool ends_with(const char *path, const char *ext) {
    size_t n = strlen(path), e = strlen(ext);
    return n > e && !strcasecmp(path + n - e, ext);
}

struct decoder *decoder_open(const char *path, struct decoder_info *info) {
    memset(info, 0, sizeof(*info));
    struct decoder *d = calloc(1, sizeof(*d));
    if (!d || !(d->data = read_file(path, &d->size))) {
        free(d);
        return NULL;
    }
    const unsigned char *p = d->data;
    bool ok = false;
    if (d->size >= 4 && !memcmp(p, "OggS", 4)) {
        int error;
        d->kind = K_VORBIS;
        d->vorbis = stb_vorbis_open_memory(p, (int)d->size, &error, NULL);
        if (d->vorbis) {
            stb_vorbis_info vi = stb_vorbis_get_info(d->vorbis);
            info->rate = vi.sample_rate;
            info->channels = vi.channels > 2 ? 2 : (unsigned)vi.channels;
            info->seconds = stb_vorbis_stream_length_in_seconds(d->vorbis);
            stb_vorbis_comment comments = stb_vorbis_get_comment(d->vorbis);
            for (int i = 0; i < comments.comment_list_length; i++) {
                vorbis_comment(comments.comment_list[i], strlen(comments.comment_list[i]), info);
            }
            ok = true;
        }
    } else if (d->size >= 4 && !memcmp(p, "fLaC", 4)) {
        d->kind = K_FLAC;
        d->info = info;
        d->flac = drflac_open_memory_with_metadata(p, d->size, flac_metadata, d, NULL);
        d->info = NULL;
        if (d->flac) {
            info->rate = d->flac->sampleRate;
            info->channels = d->flac->channels > 2 ? 2 : d->flac->channels;
            info->seconds = d->flac->sampleRate ? (double)d->flac->totalPCMFrameCount / d->flac->sampleRate : 0;
            ok = true;
        }
    } else if (d->size >= 12 && !memcmp(p, "RIFF", 4)) {
        d->kind = K_WAV;
        ok = open_wav(d, info);
        if (ok) {
            info->seconds = (double)d->pcm_frames / info->rate;
        }
    } else if (ends_with(path, ".mp3") || (d->size >= 3 && !memcmp(p, "ID3", 3)) ||
               (d->size >= 2 && p[0] == 0xff && (p[1] & 0xe0) == 0xe0)) {
        d->kind = K_MP3;
        id3v2(p, d->size, info);
        if (mp3dec_ex_open_buf(&d->mp3, p, d->size, MP3D_SEEK_TO_SAMPLE) == 0 && d->mp3.info.hz) {
            info->rate = (unsigned)d->mp3.info.hz;
            info->channels = (unsigned)d->mp3.info.channels;
            info->seconds = (double)d->mp3.samples / d->mp3.info.channels / d->mp3.info.hz;
            ok = true;
        }
    }
    if (!ok || !info->rate || !info->channels || info->channels > 2) {
        decoder_close(d);
        return NULL;
    }
    static const char *const names[] = {"WAV", "MP3", "Ogg Vorbis", "FLAC"};
    info->format = names[d->kind];
    info->stored_channels = info->channels;
    return d;
}

int decoder_read(struct decoder *d, int16_t *out, int frames, unsigned channels) {
    switch (d->kind) {
    case K_WAV:
        return read_wav(d, out, frames, channels);
    case K_MP3:
        return (int)(mp3dec_ex_read(&d->mp3, out, (size_t)frames * channels) / channels);
    case K_VORBIS:
        return stb_vorbis_get_samples_short_interleaved(d->vorbis, (int)channels, out,
                                                        frames * (int)channels);
    case K_FLAC: {
        if (d->flac->channels == channels) {
            return (int)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)frames, out);
        }
        /* (More than two channels: the first two.) */
        int16_t buffer[256 * 8];
        int done = 0;
        while (done < frames) {
            int want = frames - done < 256 ? frames - done : 256;
            int got = (int)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)want, buffer);
            for (int i = 0; i < got; i++) {
                for (unsigned c = 0; c < channels; c++) {
                    out[(done + i) * (int)channels + (int)c] = buffer[i * d->flac->channels + c];
                }
            }
            done += got;
            if (got < want) {
                break;
            }
        }
        return done;
    }
    }
    return 0;
}

bool decoder_seek(struct decoder *d, double seconds, unsigned rate, unsigned channels) {
    uint64_t frame = seconds <= 0 ? 0 : (uint64_t)(seconds * rate);
    switch (d->kind) {
    case K_WAV:
        d->position = frame < d->pcm_frames ? frame : d->pcm_frames;
        return true;
    case K_MP3:
        return mp3dec_ex_seek(&d->mp3, frame * channels) == 0;
    case K_VORBIS:
        return stb_vorbis_seek(d->vorbis, (unsigned)frame) != 0;
    case K_FLAC:
        return drflac_seek_to_pcm_frame(d->flac, frame) != 0;
    }
    return false;
}

void decoder_close(struct decoder *d) {
    if (!d) {
        return;
    }
    if (d->kind == K_MP3) {
        mp3dec_ex_close(&d->mp3);
    }
    if (d->vorbis) {
        stb_vorbis_close(d->vorbis);
    }
    if (d->flac) {
        drflac_close(d->flac);
    }
    free(d->data);
    free(d);
}
