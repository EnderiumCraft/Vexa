/* An SDL 2 program on Vexa: squares bouncing around a window (SDL's
 * renderer), a chime when it starts and a beep when the window is clicked
 * (SDL's audio), Space to pause and Escape to quit (SDL's events). Nothing in it is Vexa's own: it builds anywhere SDL does. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>

#define WIDTH 640
#define HEIGHT 400
#define SQUARES 3
#define RATE 48000

struct square {
    float x, y, dx, dy;
    int size;
    SDL_Color color;
};

/* The sound: a sine wave at `pitch` that fades out over `left` samples. */
static struct {
    SDL_AudioDeviceID device;
    double phase, pitch;
    int left, length;
} tone;

static void audio_callback(void *data, Uint8 *stream, int bytes)
{
    Sint16 *out = (Sint16 *)stream;
    int frames = bytes / (int)sizeof(Sint16);
    (void)data;
    for (int i = 0; i < frames; i++) {
        double value = 0;
        if (tone.left > 0) {
            double fade = tone.left < RATE / 20 ? tone.left / (double)(RATE / 20) : 1.0;
            value = sin(tone.phase) * 12000 * fade;
            tone.phase += 2 * M_PI * tone.pitch / RATE;
            if (tone.phase > 2 * M_PI) {
                tone.phase -= 2 * M_PI;
            }
            tone.left--;
        }
        out[i] = (Sint16)value;
    }
}

static void play(double pitch, double seconds)
{
    SDL_LockAudioDevice(tone.device);
    tone.pitch = pitch;
    tone.left = tone.length = (int)(seconds * RATE);
    SDL_UnlockAudioDevice(tone.device);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "sdl-demo: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("SDL Demo", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, WIDTH, HEIGHT,
                                          SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
    if (!renderer) {
        fprintf(stderr, "sdl-demo: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RendererInfo info;
    SDL_GetRendererInfo(renderer, &info);
    printf("sdl-demo: SDL %d.%d.%d, video %s, renderer %s\n", SDL_MAJOR_VERSION,
           SDL_MINOR_VERSION, SDL_PATCHLEVEL, SDL_GetCurrentVideoDriver(), info.name);

    SDL_AudioSpec want = { 0 }, have;
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_callback;
    tone.device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (tone.device) {
        printf("sdl-demo: audio %s, %d Hz\n", SDL_GetCurrentAudioDriver(), have.freq);
        SDL_PauseAudioDevice(tone.device, 0);
        play(660, 2.0); /* A chime to start with. */
    } else {
        printf("sdl-demo: no audio (%s)\n", SDL_GetError());
    }

    struct square squares[SQUARES] = {
        { 40, 60, 3.0f, 2.2f, 70, { 240, 80, 90, 255 } },
        { 300, 200, -2.4f, 2.8f, 90, { 90, 200, 120, 255 } },
        { 500, 80, 2.0f, -3.1f, 60, { 80, 150, 255, 255 } },
    };
    SDL_bool paused = SDL_FALSE, running = SDL_TRUE;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running = SDL_FALSE;
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = SDL_FALSE;
                } else if (event.key.keysym.sym == SDLK_SPACE) {
                    paused = !paused;
                    SDL_SetWindowTitle(window, paused ? "SDL Demo (paused)" : "SDL Demo");
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (tone.device) {
                    play(880, 0.15);
                }
                break;
            }
        }
        int w, h;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        if (!paused) {
            for (int i = 0; i < SQUARES; i++) {
                struct square *s = &squares[i];
                s->x += s->dx;
                s->y += s->dy;
                if (s->x < 0 || s->x + s->size > w) {
                    s->dx = -s->dx;
                    s->x = SDL_clamp(s->x, 0, (float)(w - s->size));
                }
                if (s->y < 0 || s->y + s->size > h) {
                    s->dy = -s->dy;
                    s->y = SDL_clamp(s->y, 0, (float)(h - s->size));
                }
            }
        }
        /* A dark gradient behind, then the squares (blended, so they mix). */
        for (int y = 0; y < h; y += 4) {
            Uint8 shade = (Uint8)(25 + 30 * y / (h ? h : 1));
            SDL_SetRenderDrawColor(renderer, shade, shade, (Uint8)(shade + 25), 255);
            SDL_Rect band = { 0, y, w, 4 };
            SDL_RenderFillRect(renderer, &band);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        for (int i = 0; i < SQUARES; i++) {
            struct square *s = &squares[i];
            SDL_SetRenderDrawColor(renderer, s->color.r, s->color.g, s->color.b, 200);
            SDL_FRect rect = { s->x, s->y, (float)s->size, (float)s->size };
            SDL_RenderFillRectF(renderer, &rect);
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    if (tone.device) {
        SDL_CloseAudioDevice(tone.device);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
