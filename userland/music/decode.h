#ifndef MUSIC_DECODE_H
#define MUSIC_DECODE_H

#include <stdbool.h>
#include <stdint.h>

/* What a sound file says about itself. */
struct decoder_info {
    unsigned rate, channels; /* (Channels: 1 or 2.) */
    unsigned stored_channels;
    double seconds;          /* How long (0 if not known). */
    const char *format;      /* "MP3", "Ogg Vorbis", "FLAC", "WAV" */
    char title[128], artist[128], album[128]; /* From its tags, or "". */
};

struct decoder;
/* Opens an MP3, Ogg Vorbis, FLAC or WAV file: NULL if it isn't one. */
struct decoder *decoder_open(const char *path, struct decoder_info *info);
/* Up to `frames` frames of 16-bit samples (`channels` of them each, as
 * info said): how many, 0 at the end. */
int decoder_read(struct decoder *d, int16_t *out, int frames, unsigned channels);
bool decoder_seek(struct decoder *d, double seconds, unsigned rate, unsigned channels);
void decoder_close(struct decoder *d);

#endif
