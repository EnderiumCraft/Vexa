/*
 * SDL's audio driver for Vexa: sound on /dev/audio0, which takes 16-bit
 * signed samples, mono or stereo, at 44100 or 48000 Hz (SDL converts the
 * rest). A write waits while the device's buffer is full, which paces SDL's
 * audio thread.
 *
 * Part of Vexa's SDK, under SDL's zlib license.
 */
#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_VEXA

#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "../SDL_sysaudio.h"

#include <vexa/syscall.h>

#define DEVICE_PATH "/dev/audio0"

/* Hidden "this" pointer for the audio functions */
#define _THIS SDL_AudioDevice *_this

struct SDL_PrivateAudioData
{
    int handle;
    Uint8 *mixbuf;
};

static void VEXAAUDIO_PlayDevice(_THIS)
{
    struct SDL_PrivateAudioData *h = _this->hidden;
    const Uint8 *p = h->mixbuf;
    Uint32 left = _this->spec.size;
    while (left) {
        long n = vx_write(h->handle, p, left);
        if (n <= 0) {
            SDL_OpenedAudioDeviceDisconnected(_this);
            return;
        }
        p += n;
        left -= (Uint32)n;
    }
}

static Uint8 *VEXAAUDIO_GetDeviceBuf(_THIS)
{
    return _this->hidden->mixbuf;
}

static void VEXAAUDIO_CloseDevice(_THIS)
{
    struct SDL_PrivateAudioData *h = _this->hidden;
    if (h->handle >= 0) {
        vx_control(h->handle, VX_AUDIO_DROP, NULL, 0);
        vx_close(h->handle);
    }
    SDL_free(h->mixbuf);
    SDL_free(h);
}

static int VEXAAUDIO_OpenDevice(_THIS, const char *devname)
{
    struct SDL_PrivateAudioData *h;
    struct vx_audio_format format;
    (void)devname;

    if (_this->iscapture) {
        return SDL_SetError("Vexa: no sound input");
    }
    h = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*h));
    if (!h) {
        return SDL_OutOfMemory();
    }
    _this->hidden = h;
    h->handle = vx_open(DEVICE_PATH, VX_OPEN_WRITE);
    if (h->handle < 0) {
        return SDL_SetError("Vexa: no sound device (%s)", DEVICE_PATH);
    }

    /* What the device plays; SDL converts what the program gives it. */
    _this->spec.format = AUDIO_S16LSB;
    _this->spec.channels = _this->spec.channels >= 2 ? 2 : 1;
    _this->spec.freq = _this->spec.freq <= 44100 ? 44100 : 48000;
    format.rate = (unsigned)_this->spec.freq;
    format.channels = _this->spec.channels;
    if (vx_control(h->handle, VX_AUDIO_SET_FORMAT, &format, sizeof(format)) < 0) {
        return SDL_SetError("Vexa: the sound device can't play %u Hz, %u channels",
                            format.rate, format.channels);
    }
    SDL_CalculateAudioSpec(&_this->spec);

    h->mixbuf = (Uint8 *)SDL_malloc(_this->spec.size);
    if (!h->mixbuf) {
        return SDL_OutOfMemory();
    }
    SDL_memset(h->mixbuf, _this->spec.silence, _this->spec.size);
    return 0;
}

static SDL_bool VEXAAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = VEXAAUDIO_OpenDevice;
    impl->PlayDevice = VEXAAUDIO_PlayDevice;
    impl->GetDeviceBuf = VEXAAUDIO_GetDeviceBuf;
    impl->CloseDevice = VEXAAUDIO_CloseDevice;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    impl->SupportsNonPow2Samples = SDL_TRUE;
    return SDL_TRUE;
}

AudioBootStrap VEXAAUDIO_bootstrap = {
    "vexa", "Vexa /dev/audio0", VEXAAUDIO_Init, SDL_FALSE
};

#endif /* SDL_AUDIO_DRIVER_VEXA */
