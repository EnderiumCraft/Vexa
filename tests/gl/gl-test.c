/* OpenGL on Vexa, with Mesa: draws a triangle with a shader and reads the
 * pixels back.
 *
 *   gl-test      through OSMesa: into memory, no X needed
 *   gl-test x    through GLX: in an X window (DISPLAY must be set)
 *
 * Prints the renderer (llvmpipe: shaders compiled to machine code by LLVM),
 * and "gl-test: passed" if the pixels are right. */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/osmesa.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 128

static const char *vertex_source =
    "#version 120\n"
    "attribute vec2 position;\n"
    "varying vec3 color;\n"
    "void main() {\n"
    "    color = vec3(1.0, 0.5, 0.0);\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char *fragment_source =
    "#version 120\n"
    "varying vec3 color;\n"
    "void main() { gl_FragColor = vec4(color, 1.0); }\n";

static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        printf("gl-test: shader: %s\n", log);
    }
    return shader;
}

/* Draws, then checks the middle (the triangle) and a corner (the clear colour). */
static int draw_and_check(void) {
    printf("gl-test: %s, %s, OpenGL %s\n", (const char *)glGetString(GL_VENDOR),
           (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_source));
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    glUseProgram(program);

    static const GLfloat triangle[] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};
    glViewport(0, 0, SIZE, SIZE);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, triangle);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    unsigned char middle[4], corner[4];
    glReadPixels(SIZE / 2, SIZE / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, middle);
    glReadPixels(2, SIZE - 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    printf("gl-test: middle %d,%d,%d corner %d,%d,%d\n", middle[0], middle[1], middle[2],
           corner[0], corner[1], corner[2]);
    int ok = middle[0] == 255 && middle[1] >= 126 && middle[1] <= 129 && middle[2] == 0 &&
             corner[0] == 0 && corner[1] == 0 && corner[2] == 255 && glGetError() == GL_NO_ERROR;
    return ok;
}

static int test_osmesa(void) {
    OSMesaContext context = OSMesaCreateContextExt(OSMESA_RGBA, 24, 0, 0, NULL);
    if (!context) {
        printf("gl-test: no OSMesa context\n");
        return 0;
    }
    void *buffer = calloc(SIZE * SIZE, 4);
    if (!OSMesaMakeCurrent(context, buffer, GL_UNSIGNED_BYTE, SIZE, SIZE)) {
        printf("gl-test: OSMesaMakeCurrent failed\n");
        return 0;
    }
    int ok = draw_and_check();
    OSMesaDestroyContext(context);
    free(buffer);
    return ok;
}

static int test_glx(void) {
    printf("gl-test: opening %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(no DISPLAY)");
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        printf("gl-test: no X display\n");
        return 0;
    }
    int attributes[] = {GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                        GLX_DOUBLEBUFFER, None};
    XVisualInfo *visual = glXChooseVisual(display, DefaultScreen(display), attributes);
    if (!visual) {
        printf("gl-test: no GLX visual\n");
        return 0;
    }
    printf("gl-test: visual 0x%lx, depth %d\n", (unsigned long)visual->visualid, visual->depth);
    Window rootwin = RootWindow(display, visual->screen);
    XSetWindowAttributes wa = {0};
    wa.colormap = XCreateColormap(display, rootwin, visual->visual, AllocNone);
    wa.event_mask = ExposureMask | StructureNotifyMask;
    /* (A visual of another depth than the root's needs its own border pixel.) */
    Window window = XCreateWindow(display, rootwin, 0, 0, SIZE, SIZE, 0, visual->depth,
                                  InputOutput, visual->visual,
                                  CWColormap | CWEventMask | CWBorderPixel, &wa);
    XStoreName(display, window, "gl-test");
    XMapWindow(display, window);
    XSync(display, False);
    printf("gl-test: X window %lu\n", (unsigned long)window);
    GLXContext context = glXCreateContext(display, visual, NULL, True);
    if (!context || !glXMakeCurrent(display, window, context)) {
        printf("gl-test: no GLX context\n");
        return 0;
    }
    int ok = draw_and_check();
    glXSwapBuffers(display, window);
    XSync(display, False);
    glXMakeCurrent(display, None, NULL);
    glXDestroyContext(display, context);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return ok;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int ok = argc > 1 && !strcmp(argv[1], "x") ? test_glx() : test_osmesa();
    printf(ok ? "gl-test: passed\n" : "gl-test: FAILED\n");
    return ok ? 0 : 1;
}
