/* sdl-gl-test: OpenGL in a native Vexa program, through SDL (Vexa's driver
 * loads Mesa's libOSMesa.so, softpipe). A window with an OpenGL context: a
 * red clear and a green triangle (read back with glReadPixels), then a
 * spinning, shaded triangle for a moment (or until the window is closed,
 * with --spin). Prints "sdl-gl-test: passed" and the frames per second. */
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 320
#define HEIGHT 240

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("sdl-gl-test: FAILED: %s\n", what);
        failures++;
    }
}

static void pixel(int x, int y, unsigned char rgba[4]) {
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

static void triangle(float angle) {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glRotatef(angle, 0, 0, 1);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0.3f, 0.3f);
    glVertex2f(0, 0.8f);
    glColor3f(0.3f, 1, 0.3f);
    glVertex2f(-0.7f, -0.6f);
    glColor3f(0.3f, 0.3f, 1);
    glVertex2f(0.7f, -0.6f);
    glEnd();
}

int main(int argc, char **argv) {
    int spin = argc > 1 && strcmp(argv[1], "--spin") == 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("sdl-gl-test: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_Window *window = SDL_CreateWindow("OpenGL", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          WIDTH, HEIGHT, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
    if (!context) {
        printf("sdl-gl-test: no OpenGL: %s\n", SDL_GetError());
        return 1;
    }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    printf("sdl-gl-test: %s, OpenGL %s\n", renderer ? renderer : "?", version ? version : "?");
    check(renderer && strstr(renderer, "softpipe"), "the renderer is Mesa's softpipe");

    /* A red clear, a green triangle over the middle (not the corner). */
    glViewport(0, 0, WIDTH, HEIGHT);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glBegin(GL_TRIANGLES);
    glColor3f(0, 1, 0);
    glVertex2f(-0.5f, -0.5f);
    glVertex2f(0.5f, -0.5f);
    glVertex2f(0, 0.5f);
    glEnd();
    glFinish();
    unsigned char middle[4], corner[4];
    pixel(WIDTH / 2, HEIGHT / 2, middle);
    pixel(2, 2, corner);
    check(middle[0] == 0 && middle[1] == 255 && middle[2] == 0, "the triangle is green");
    check(corner[0] == 255 && corner[1] == 0 && corner[2] == 0, "the clear color is red");
    check(glGetError() == GL_NO_ERROR, "no OpenGL errors");
    SDL_GL_SwapWindow(window);

    /* Spinning: a second (or until closed, with --spin). */
    Uint32 start = SDL_GetTicks(), frames = 0;
    for (int running = 1; running;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                running = 0;
            }
        }
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.15f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        triangle((SDL_GetTicks() - start) * 0.09f);
        SDL_GL_SwapWindow(window);
        frames++;
        if (!spin && SDL_GetTicks() - start >= 1000) {
            running = 0;
        }
    }
    Uint32 elapsed = SDL_GetTicks() - start;
    printf("sdl-gl-test: %u frames in %u ms\n", (unsigned)frames, (unsigned)elapsed);
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) {
        printf("sdl-gl-test: %d checks failed\n", failures);
        return 1;
    }
    printf("sdl-gl-test: passed\n");
    return 0;
}
