/* OpenGL 3.2 core profile, through OSMesa: the version WebRender needs. Draws a
 * triangle with #version 150 shaders into memory, reads the pixels back, and
 * checks them.
 *
 *   core-gl-test    prints the GL version and renderer, then
 *                   "core-gl-test: passed" if the context is core 3.2 or later
 *                   and the pixels are right
 *
 * Built with the SDK (vexa-cc) and linked to libOSMesa.so. The compatibility
 * profile (gl-test) is not enough here: WebRender needs the core profile. */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/osmesa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 128

static const char *vertex_source =
    "#version 150\n"
    "in vec2 position;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char *fragment_source =
    "#version 150\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    color = vec4(1.0, 0.5, 0.0, 1.0);\n"
    "}\n";

static int failed;

static void check(const char *what) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("core-gl-test: %s: GL error 0x%x\n", what, error);
        failed = 1;
    }
}

static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        printf("core-gl-test: shader: %s\n", log);
        failed = 1;
    }
    return shader;
}

int main(void) {
    static const int attribs[] = {
        OSMESA_FORMAT, OSMESA_RGBA,
        OSMESA_DEPTH_BITS, 24,
        OSMESA_PROFILE, OSMESA_CORE_PROFILE,
        OSMESA_CONTEXT_MAJOR_VERSION, 3,
        OSMESA_CONTEXT_MINOR_VERSION, 2,
        0,
    };
    OSMesaContext context = OSMesaCreateContextAttribs(attribs, NULL);
    if (!context) {
        printf("core-gl-test: no OSMesa core 3.2 context\n");
        return 1;
    }
    unsigned char *pixels = calloc(SIZE * SIZE, 4);
    if (!pixels || !OSMesaMakeCurrent(context, pixels, GL_UNSIGNED_BYTE, SIZE, SIZE)) {
        printf("core-gl-test: OSMesaMakeCurrent failed\n");
        return 1;
    }

    printf("core-gl-test: %s, %s, OpenGL %s, GLSL %s\n",
           (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION),
           (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    check("version query");
    int core = major > 3 || (major == 3 && minor >= 2);
    printf("core-gl-test: context is %d.%d, %s\n", major, minor,
           core ? "core 3.2 or later" : "older than 3.2");
    failed |= !core;

    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_source));
    glBindFragDataLocation(program, 0, "color");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        printf("core-gl-test: link failed\n");
        failed = 1;
    }
    glUseProgram(program);

    /* Core profile: vertices come from a vertex array object. */
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    static const GLfloat triangle[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
    GLint position = glGetAttribLocation(program, "position");
    glEnableVertexAttribArray(position);
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    check("vertex setup");

    glViewport(0, 0, SIZE, SIZE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    check("draw");

    /* OSMesa's rows are bottom-up, but the middle of the image is the same either way. */
    unsigned char *middle = pixels + ((SIZE / 2) * SIZE + SIZE / 2) * 4;
    printf("core-gl-test: middle %d,%d,%d\n", middle[0], middle[1], middle[2]);
    int right = abs(middle[0] - 255) <= 2 && abs(middle[1] - 128) <= 2 && middle[2] <= 2;
    if (!right) {
        printf("core-gl-test: pixels are wrong\n");
        failed = 1;
    }

    OSMesaDestroyContext(context);
    free(pixels);
    if (failed) {
        return 1;
    }
    printf("core-gl-test: passed\n");
    return 0;
}
