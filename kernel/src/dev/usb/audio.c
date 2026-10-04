#include <vexa/device.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/sound.h>
#include <vexa/string.h>
#include <vexa/usb.h>

/*
 * USB sound cards, headsets and speakers (USB Audio Class 1): an output of
 * the sound core. The audio streaming interface's alternate settings each
 * describe a format (channels, sample size, rates) and an isochronous OUT
 * endpoint; this picks 16-bit stereo (or mono) at 48 kHz (or 44.1), selects
 * it with SET_INTERFACE, sets the rate, and keeps the endpoint fed: every
 * millisecond (a full speed frame) a packet of what the core mixes.
 *
 * Isochronous transfers are xHCI's here; on older controllers the card is
 * named but doesn't play.
 */

#define CS_INTERFACE 0x24
#define AS_GENERAL 0x01
#define FORMAT_TYPE 0x02
#define SET_CUR 0x01
#define SET_INTERFACE 11

struct usb_audio {
    struct sound_output output;
    struct usb_device *device;
    struct usb_endpoint endpoint;
    unsigned channels, rate;
    unsigned remainder; /* Frames' fractions carried from packet to packet (44.1 kHz). */
    int16_t *mix;
};

/* One packet: a millisecond's worth of frames (or, at high speed, an
 * interval's), mixed by the core. */
static int fill(void *arg, uint8_t *data, int max) {
    struct usb_audio *a = arg;
    a->remainder += a->rate;
    unsigned frames = a->remainder / 1000;
    a->remainder %= 1000;
    unsigned frame_bytes = a->channels * 2;
    if (frames * frame_bytes > (unsigned)max) {
        frames = (unsigned)max / frame_bytes;
    }
    sound_mix(&a->output, a->mix, frames);
    int16_t *out = (int16_t *)data;
    for (unsigned i = 0; i < frames; i++) {
        if (a->channels == 2) {
            out[i * 2] = a->mix[i * 2];
            out[i * 2 + 1] = a->mix[i * 2 + 1];
        } else {
            out[i] = (int16_t)((a->mix[i * 2] + a->mix[i * 2 + 1]) / 2);
        }
    }
    return (int)(frames * frame_bytes);
}

static void audio_start(struct sound_output *output) {
    (void)output; /* (It sends all the time, silence when there's nothing.) */
}

static uint32_t rate_at(const uint8_t *p) {
    return p[0] | p[1] << 8 | (uint32_t)p[2] << 16;
}

/* The alternate settings of the interface: the best 16-bit PCM format with
 * an isochronous OUT endpoint. */
static int choose(struct usb_interface *interface, struct usb_endpoint *endpoint,
                  unsigned *channels, unsigned *rate, bool *set_rate) {
    const uint8_t *p = interface->device->config;
    const uint8_t *end = p + interface->device->config_length;
    int best = -1, best_score = -1, alternate = -1, score = -1;
    unsigned ch = 0, r = 0;
    bool many_rates = false, pcm = false;
    struct usb_endpoint ep = {0};
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_DESC_INTERFACE && p[0] >= 9) {
            alternate = p[2] == interface->number && p[3] != 0 ? p[3] : -1;
            score = -1;
            pcm = false;
        } else if (alternate >= 0 && p[1] == CS_INTERFACE && p[2] == AS_GENERAL && p[0] >= 7) {
            pcm = (p[5] | p[6] << 8) == 1; /* wFormatTag: PCM */
        } else if (alternate >= 0 && p[1] == CS_INTERFACE && p[2] == FORMAT_TYPE && p[0] >= 8 &&
                   p[3] == 1 && pcm) {
            unsigned nr = p[4], subframe = p[5], bits = p[6], kinds = p[7];
            if ((nr == 1 || nr == 2) && subframe == 2 && bits == 16) {
                /* Rates: a list, or (kinds 0) a range. */
                unsigned pick = 0;
                if (kinds == 0 && p[0] >= 14) {
                    uint32_t low = rate_at(p + 8), high = rate_at(p + 11);
                    pick = low <= 48000 && high >= 48000 ? 48000 : low <= 44100 && high >= 44100 ? 44100 : 0;
                } else {
                    for (unsigned i = 0; i < kinds && 8 + i * 3 + 3 <= p[0]; i++) {
                        uint32_t f = rate_at(p + 8 + i * 3);
                        if (f == 48000 || (f == 44100 && pick != 48000)) {
                            pick = f;
                        }
                    }
                }
                if (pick) {
                    ch = nr, r = pick;
                    many_rates = kinds != 1;
                    score = (nr == 2 ? 2 : 0) + (pick == 48000 ? 1 : 0);
                }
            }
        } else if (alternate >= 0 && p[1] == USB_DESC_ENDPOINT && p[0] >= 7 && score >= 0 &&
                   (p[3] & 3) == USB_ENDPOINT_ISOCHRONOUS && !(p[2] & USB_DIR_IN)) {
            if (score > best_score) {
                best = alternate, best_score = score;
                ep.address = p[2];
                ep.type = USB_ENDPOINT_ISOCHRONOUS;
                ep.max_packet = (uint16_t)(p[4] | p[5] << 8);
                ep.interval = p[6];
                *endpoint = ep;
                *channels = ch;
                *rate = r;
                *set_rate = many_rates;
            }
        }
        p += p[0];
    }
    return best;
}

static bool audio_probe(struct usb_interface *interface) {
    if (interface->interface_class != 1 || interface->subclass != 2 || interface->protocol != 0) {
        return false; /* (Audio streaming, version 1.) */
    }
    struct usb_audio *a = kzalloc(sizeof(*a));
    if (!a) {
        return false;
    }
    bool set_rate = false;
    int alternate = choose(interface, &a->endpoint, &a->channels, &a->rate, &set_rate);
    char name[64];
    usb_device_name(interface->device, name, sizeof(name));
    if (alternate < 0) {
        kprintf("[usb-audio] %s: no 16-bit output it can play\n", name);
        kfree(a);
        return false;
    }
    a->device = interface->device;
    a->mix = kmalloc(1024 * 4);
    if (!a->mix) {
        kfree(a);
        return false;
    }
    usb_control(a->device, USB_DIR_OUT | 0x01, SET_INTERFACE, (uint16_t)alternate, interface->number,
                NULL, 0);
    if (set_rate) {
        uint8_t f[3] = {(uint8_t)a->rate, (uint8_t)(a->rate >> 8), (uint8_t)(a->rate >> 16)};
        usb_control(a->device, USB_DIR_OUT | 0x22, SET_CUR, 0x0100, a->endpoint.address, f, 3);
    }
    ksnprintf(a->output.name, sizeof(a->output.name), "%s", name[0] ? name : "USB audio");
    a->output.rate = a->rate;
    a->output.start = audio_start;
    a->output.data = a;
    uint16_t packet = (uint16_t)((a->rate + 999) / 1000 * a->channels * 2);
    if (packet > (a->endpoint.max_packet & 0x7ff)) {
        packet = a->endpoint.max_packet & 0x7ff;
    }
    a->output.queued = a->rate * 16 / 1000; /* (The packets kept queued.) */
    int error = usb_iso_out(a->device, &a->endpoint, packet, fill, a);
    if (error) {
        kprintf("[usb-audio] %s: can't send to it (%s)\n", name,
                error == -VX_ENOSYS ? "isochronous transfers need an xHCI controller" : "error");
        kfree(a->mix);
        kfree(a);
        return false;
    }
    interface->driver_data = a;
    kprintf("[usb-audio] %s: %u Hz, %s, 16-bit\n", name, a->rate,
            a->channels == 2 ? "stereo" : "mono");
    sound_register(&a->output);
    return true;
}

static void audio_disconnect(struct usb_interface *interface) {
    struct usb_audio *a = interface->driver_data;
    if (!a) {
        return;
    }
    sound_unregister(&a->output);
    usb_iso_stop(a->device, a->endpoint.address);
    interface->driver_data = NULL;
    /* (The structure stays: the sound core may still be looking at it.) */
}

const struct usb_driver usb_audio_driver = {
    .name = "usb-audio",
    .probe = audio_probe,
    .disconnect = audio_disconnect,
};
