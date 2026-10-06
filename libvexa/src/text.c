#include <stdlib.h>
#include <string.h>
#include <vexa/font.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

/*
 * Text: TrueType fonts (DejaVu, in /share/fonts), drawn smooth with
 * stb_truetype, and UTF-8. Each font (a face at a size) keeps the glyphs it
 * has drawn. Without the font files, text falls back to the console's
 * bitmap font.
 */

/* stb_truetype, with what it needs from a C library. The math other than
 * floor, ceil and sqrt is only for signed distance fields, which Vexa
 * doesn't make. */
static int text_floor(double x) {
    int i = (int)x;
    return x < i ? i - 1 : i;
}
static int text_ceil(double x) {
    int i = (int)x;
    return x > i ? i + 1 : i;
}
static double text_sqrt(double x) {
    __asm__("sqrtsd %1, %0" : "=x"(x) : "x"(x));
    return x;
}
static double text_unused(double x, double y) {
    (void)y;
    return x;
}
#define STBTT_ifloor(x) text_floor(x)
#define STBTT_iceil(x) text_ceil(x)
#define STBTT_sqrt(x) text_sqrt(x)
#define STBTT_pow(x, y) text_unused(x, y)
#define STBTT_fmod(x, y) text_unused(x, y)
#define STBTT_cos(x) text_unused(x, 0)
#define STBTT_acos(x) text_unused(x, 0)
#define STBTT_fabs(x) ((x) < 0 ? -(x) : (x))
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u) ((void)(u), free(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "../../third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

static const char *const face_files[VX_FACE_COUNT] = {
    "/share/fonts/DejaVuSans.ttf",
    "/share/fonts/DejaVuSans-Bold.ttf",
    "/share/fonts/DejaVuSansMono.ttf",
};

/* A font file, mapped (or read) once per program. */
struct face {
    bool tried, ok;
    stbtt_fontinfo info;
    int ascent, descent, gap; /* In font units. */
};
static struct face faces[VX_FACE_COUNT];

static struct face *load_face(int which) {
    struct face *f = &faces[which];
    if (f->tried) {
        return f->ok ? f : NULL;
    }
    f->tried = true;
    struct vx_stat st;
    if (vx_stat(face_files[which], &st) < 0 || st.size < 64) {
        return NULL;
    }
    int handle = vx_open(face_files[which], VX_OPEN_READ);
    if (handle < 0) {
        return NULL;
    }
    size_t size = (size_t)st.size;
    unsigned char *data = vx_map_file(handle, 0, (size + 4095) & ~(size_t)4095, 0);
    if (!data) {
        /* No mapping (a file system without it): read it all. */
        data = malloc(size);
        long got = 0;
        while (data && (size_t)got < size) {
            long n = vx_read(handle, data + got, size - (size_t)got);
            if (n <= 0) {
                free(data);
                data = NULL;
                break;
            }
            got += n;
        }
    }
    vx_close(handle);
    if (!data || !stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        return NULL;
    }
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->gap);
    f->ok = true;
    return f;
}

struct glyph {
    uint32_t codepoint; /* 0: an empty slot. */
    int16_t x0, y0, width, height; /* The bitmap, from the pen on the baseline. */
    float advance;
    uint8_t *alpha;
};

struct vx_font {
    int face, size;
    struct face *file; /* NULL: the bitmap font. */
    float scale;
    int ascent, height; /* Pixels: the baseline below the top, and a line. */
    float mono_advance; /* For the monospaced face: every character's. */
    struct glyph *glyphs;
    int glyph_slots, glyph_count;
};

#define MAX_FONTS 16
static struct vx_font fonts[MAX_FONTS];
static int font_count;

const struct vx_font *vx_font(int face, int size) {
    if (face < 0 || face >= VX_FACE_COUNT) {
        face = VX_FACE_SANS;
    }
    size = size < 6 ? 6 : size > 200 ? 200 : size;
    for (int i = 0; i < font_count; i++) {
        if (fonts[i].face == face && fonts[i].size == size) {
            return &fonts[i];
        }
    }
    struct vx_font *f = &fonts[font_count < MAX_FONTS ? font_count++ : MAX_FONTS - 1];
    if (f->glyphs) { /* Out of slots: reuse the last (rare). */
        for (int i = 0; i < f->glyph_slots; i++) {
            free(f->glyphs[i].alpha);
        }
        free(f->glyphs);
    }
    memset(f, 0, sizeof(*f));
    f->face = face;
    f->size = size;
    f->file = load_face(face);
    if (!f->file) {
        f->ascent = 12;
        f->height = FONT_HEIGHT;
        f->mono_advance = FONT_WIDTH;
        return f;
    }
    f->scale = stbtt_ScaleForMappingEmToPixels(&f->file->info, (float)size);
    f->ascent = text_ceil(f->file->ascent * f->scale - 0.5);
    f->height = text_ceil((f->file->ascent - f->file->descent) * f->scale);
    if (face == VX_FACE_MONO) {
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&f->file->info, 'M', &advance, &bearing);
        f->mono_advance = (float)text_floor(advance * f->scale + 0.5);
    }
    return f;
}

const struct vx_font *vx_font_ui(void) {
    return vx_font(VX_FACE_SANS, VX_UI_FONT_SIZE);
}

int vx_font_height(const struct vx_font *font) {
    return font->height;
}

int vx_font_ascent(const struct vx_font *font) {
    return font->ascent;
}

/* Darker, crisper edges than linear coverage (unhinted text looks thin). */
static uint8_t coverage[256];

static void make_coverage(void) {
    for (int a = 0; a < 256; a++) {
        int boosted = (int)text_sqrt((double)a * 255.0);
        coverage[a] = (uint8_t)((a + boosted * 2) / 3);
    }
}

static struct glyph *render_glyph(struct vx_font *f, uint32_t codepoint, struct glyph *g) {
    g->codepoint = codepoint;
    struct face *file = f->file;
    float scale = f->scale;
    int index = stbtt_FindGlyphIndex(&file->info, (int)codepoint);
    if (!index) {
        /* Another face may have it (the monospaced one has box drawing). */
        for (int other = 0; other < VX_FACE_COUNT && !index; other++) {
            struct face *o = load_face(other);
            if (o && o != file) {
                int i = stbtt_FindGlyphIndex(&o->info, (int)codepoint);
                if (i) {
                    file = o;
                    index = i;
                    scale = stbtt_ScaleForMappingEmToPixels(&o->info, (float)f->size);
                }
            }
        }
    }
    int advance, bearing;
    stbtt_GetGlyphHMetrics(&file->info, index, &advance, &bearing);
    g->advance = f->mono_advance ? f->mono_advance : advance * scale;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&file->info, index, scale, scale, &x0, &y0, &x1, &y1);
    if (f->mono_advance && file != f->file) {
        /* Centered in the cell. */
        int width = x1 - x0;
        int shift = ((int)f->mono_advance - width) / 2 - x0;
        x0 += shift;
        x1 += shift;
    }
    g->x0 = (int16_t)x0;
    g->y0 = (int16_t)y0;
    g->width = (int16_t)(x1 - x0);
    g->height = (int16_t)(y1 - y0);
    if (g->width > 0 && g->height > 0) {
        g->alpha = malloc((size_t)g->width * g->height);
        if (g->alpha) {
            stbtt_MakeGlyphBitmap(&file->info, g->alpha, g->width, g->height, g->width, scale,
                                  scale, index);
            for (int i = 0; i < g->width * g->height; i++) {
                g->alpha[i] = coverage[g->alpha[i]];
            }
        }
    }
    f->glyph_count++;
    return g;
}

static struct glyph *find_glyph(struct vx_font *f, uint32_t codepoint) {
    if (!coverage[255]) {
        make_coverage();
    }
    if (f->glyph_count * 2 >= f->glyph_slots) {
        int slots = f->glyph_slots ? f->glyph_slots * 2 : 256;
        struct glyph *table = calloc((size_t)slots, sizeof(*table));
        if (!table) {
            return NULL;
        }
        for (int i = 0; i < f->glyph_slots; i++) {
            if (f->glyphs[i].codepoint) {
                uint32_t h = f->glyphs[i].codepoint * 2654435761u % (uint32_t)slots;
                while (table[h].codepoint) {
                    h = (h + 1) % (uint32_t)slots;
                }
                table[h] = f->glyphs[i];
            }
        }
        free(f->glyphs);
        f->glyphs = table;
        f->glyph_slots = slots;
    }
    uint32_t h = codepoint * 2654435761u % (uint32_t)f->glyph_slots;
    while (f->glyphs[h].codepoint) {
        if (f->glyphs[h].codepoint == codepoint) {
            return &f->glyphs[h];
        }
        h = (h + 1) % (uint32_t)f->glyph_slots;
    }
    return render_glyph(f, codepoint, &f->glyphs[h]);
}

/* ---- UTF-8 ---- */

uint32_t vx_utf8_next(const char **text) {
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t c = *p++;
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80 && c < 0xc0) {
        *text = (const char *)p;
        return 0xfffd; /* A stray continuation byte. */
    }
    c &= more == 3 ? 0x07 : more == 2 ? 0x0f : more == 1 ? 0x1f : 0x7f;
    for (; more > 0; more--) {
        if ((*p & 0xc0) != 0x80) {
            *text = (const char *)p;
            return 0xfffd;
        }
        c = c << 6 | (*p++ & 0x3f);
    }
    *text = (const char *)p;
    return c;
}

int vx_utf8_encode(uint32_t c, char out[4]) {
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xc0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | c >> 18);
    out[1] = (char)(0x80 | (c >> 12 & 0x3f));
    out[2] = (char)(0x80 | (c >> 6 & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

size_t vx_utf8_previous(const char *text, size_t at) {
    while (at > 0) {
        at--;
        if (((unsigned char)text[at] & 0xc0) != 0x80) {
            break;
        }
    }
    return at;
}

/* ---- Drawing ---- */

/* `a` (0 to 256) of src over dst: see widgets.c (a masked difference borrows
 * between red and blue when src is darker, and tints the result). */
static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a) {
    uint32_t rb = (((dst & 0xff00ff) * (256 - a) + (src & 0xff00ff) * a) >> 8) & 0xff00ff;
    uint32_t g = (((dst & 0x00ff00) * (256 - a) + (src & 0x00ff00) * a) >> 8) & 0x00ff00;
    return rb | g;
}

static void draw_bitmap_char(struct vx_surface *s, int x, int y, uint32_t c, uint32_t fg) {
    if (c < FONT_FIRST_CHAR || c >= FONT_FIRST_CHAR + FONT_GLYPH_COUNT) {
        c = '?';
    }
    const uint8_t *rows = font_glyphs[c - FONT_FIRST_CHAR];
    for (int r = 0; r < FONT_HEIGHT; r++) {
        int py = y + r;
        if (py < 0 || py >= s->height) {
            continue;
        }
        for (int col = 0; col < FONT_WIDTH; col++) {
            int px = x + col;
            if (px >= 0 && px < s->width && (rows[r] & (0x80 >> col))) {
                s->pixels[(long)py * s->stride + px] = fg;
            }
        }
    }
}

static void draw_glyph(struct vx_surface *s, int pen_x, int baseline, const struct glyph *g,
                       uint32_t fg) {
    if (!g->alpha) {
        return;
    }
    int left = pen_x + g->x0, top = baseline + g->y0;
    int c0 = left < 0 ? -left : 0, r0 = top < 0 ? -top : 0;
    int c1 = left + g->width > s->width ? s->width - left : g->width;
    int r1 = top + g->height > s->height ? s->height - top : g->height;
    fg &= 0xffffff;
    for (int r = r0; r < r1; r++) {
        uint32_t *line = s->pixels + (long)(top + r) * s->stride + left;
        const uint8_t *alpha = g->alpha + r * g->width;
        for (int c = c0; c < c1; c++) {
            unsigned a = alpha[c];
            if (a == 255) {
                line[c] = fg;
            } else if (a) {
                line[c] = blend(line[c], fg, a + (a >> 7));
            }
        }
    }
}

static int measure(const struct vx_font *font, const char *text, size_t length) {
    struct vx_font *f = (struct vx_font *)font;
    const char *end = text + length;
    if (!f->file) {
        int n = 0;
        while (text < end && *text) {
            vx_utf8_next(&text);
            n++;
        }
        return n * FONT_WIDTH;
    }
    float x = 0;
    while (text < end && *text) {
        struct glyph *g = find_glyph(f, vx_utf8_next(&text));
        if (g) {
            x += g->advance;
        }
    }
    return text_ceil(x - 0.01);
}

int vx_text_width_font(const struct vx_font *font, const char *text) {
    return measure(font, text, strlen(text));
}

int vx_text_width_bytes(const struct vx_font *font, const char *text, size_t length) {
    return measure(font, text, length);
}

int vx_text_width(const char *text) {
    return measure(vx_font_ui(), text, strlen(text));
}

size_t vx_text_fit_bytes(const struct vx_font *font, const char *text, int width) {
    struct vx_font *f = (struct vx_font *)font;
    const char *p = text;
    float x = 0;
    while (*p) {
        const char *start = p;
        uint32_t c = vx_utf8_next(&p);
        struct glyph *g = f->file ? find_glyph(f, c) : NULL;
        x += g ? g->advance : FONT_WIDTH;
        if (x > (float)width + 0.01f) {
            return (size_t)(start - text);
        }
    }
    return (size_t)(p - text);
}

int vx_text(struct vx_surface *s, const struct vx_font *font, int x, int y, const char *text,
            uint32_t fg, uint32_t bg) {
    struct vx_font *f = (struct vx_font *)font;
    if (bg != VX_TRANSPARENT) {
        vx_fill(s, x, y, vx_text_width_font(font, text), f->height, bg);
    }
    if (!f->file) {
        while (*text) {
            draw_bitmap_char(s, x, y, vx_utf8_next(&text), fg);
            x += FONT_WIDTH;
        }
        return x;
    }
    float pen = (float)x;
    int baseline = y + f->ascent;
    while (*text) {
        uint32_t c = vx_utf8_next(&text);
        struct glyph *g = find_glyph(f, c);
        if (!g) {
            continue;
        }
        if (c != ' ' && pen < s->width && pen + g->advance >= 0) {
            draw_glyph(s, text_floor(pen + 0.5), baseline, g, fg);
        }
        pen += g->advance;
    }
    return text_ceil(pen - 0.01);
}

int vx_draw_text(struct vx_surface *s, int x, int y, const char *text, uint32_t fg, uint32_t bg) {
    const struct vx_font *f = vx_font_ui();
    /* Centered in the 16-pixel line the bitmap font had. */
    return vx_text(s, f, x, y + (VX_LINE_HEIGHT - f->height) / 2, text, fg, bg);
}

void vx_draw_char(struct vx_surface *s, int x, int y, uint32_t c, uint32_t fg, uint32_t bg) {
    struct vx_font *f = (struct vx_font *)vx_font(VX_FACE_MONO, VX_MONO_FONT_SIZE);
    if (bg != VX_TRANSPARENT) {
        vx_fill(s, x, y, VX_CELL_WIDTH, VX_LINE_HEIGHT, bg);
    }
    if (c <= ' ') {
        return;
    }
    if (!f->file) {
        draw_bitmap_char(s, x, y, c, fg);
        return;
    }
    struct glyph *g = find_glyph(f, c);
    if (g) {
        draw_glyph(s, x, y + (VX_LINE_HEIGHT - f->height) / 2 + f->ascent, g, fg);
    }
}

int vx_font_cell_width(const struct vx_font *font) {
    struct vx_font *f = (struct vx_font *)font;
    if (f->mono_advance) {
        return (int)f->mono_advance;
    }
    struct glyph *g = f->file ? find_glyph(f, 'M') : NULL;
    return g ? text_ceil(g->advance) : FONT_WIDTH;
}

void vx_draw_cell(struct vx_surface *s, const struct vx_font *font, int x, int y, int width,
                  int height, uint32_t c, uint32_t fg, uint32_t bg) {
    struct vx_font *f = (struct vx_font *)font;
    if (bg != VX_TRANSPARENT) {
        vx_fill(s, x, y, width, height, bg);
    }
    if (c <= ' ') {
        return;
    }
    if (!f->file) {
        draw_bitmap_char(s, x, y, c, fg);
        return;
    }
    struct glyph *g = find_glyph(f, c);
    if (g) {
        draw_glyph(s, x, y + (height - f->height) / 2 + f->ascent, g, fg);
    }
}
