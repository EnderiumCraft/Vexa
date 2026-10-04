#ifndef VEXA_SOUND_H
#define VEXA_SOUND_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Sound outputs, and what plays on them (dev/sound.c). Programs write to
 * /dev/audio0: each handle is a stream, with its own format. The current
 * output pulls the streams mixed (resampled to its rate, at the system's
 * volume) with sound_mix, as fast as it plays them.
 */

struct sound_output {
    char name[48];
    unsigned rate;     /* Frames a second it plays (stereo, 16-bit). */
    /* Something is to play: start pulling. Called with the core's lock
     * held, so it only takes note (no locks, no sleeping); pulling with
     * nothing to play is fine, and an output may stop by itself after a
     * while of silence. */
    void (*start)(struct sound_output *output);
    void *data;
    /* Set by the output: frames it has taken from sound_mix that haven't
     * been heard yet (what's in its buffer). */
    volatile unsigned queued;
    /* The core's. */
    unsigned id;
    struct sound_output *next;
};

/* A new output (HD Audio at boot, a USB sound card when plugged in): it
 * becomes the current one. */
void sound_register(struct sound_output *output);
/* An output is gone (unplugged): the previous one plays instead. */
void sound_unregister(struct sound_output *output);
/* For the current output: the next `frames` stereo frames at its rate.
 * Returns false (and silence) if nothing is playing. Others get silence. */
bool sound_mix(struct sound_output *output, int16_t *out, unsigned frames);

#endif
