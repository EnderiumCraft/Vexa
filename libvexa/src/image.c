#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

/*
 * Images: PNG (8-bit gray, gray+alpha, RGB, RGBA and palette, not
 * interlaced), BMP (24 and 32 bits, uncompressed) and PPM (P6), decoded
 * into 0xRRGGBB pixels. Alpha is blended onto a background color, or kept
 * (VX_IMAGE_ALPHA: 0xAARRGGBB).
 *
 * PNG data is zlib-compressed: inflate() below is a small decoder of
 * DEFLATE (RFC 1951), with the fixed and dynamic Huffman codes.
 */

/* ---- DEFLATE ---- */

struct bits {
    const uint8_t *data;
    size_t size, at;
    uint32_t buffer;
    int count;
};

static int get_bit(struct bits *b) {
    if (b->count == 0) {
        if (b->at >= b->size) {
            return -1;
        }
        b->buffer = b->data[b->at++];
        b->count = 8;
    }
    int bit = b->buffer & 1;
    b->buffer >>= 1;
    b->count--;
    return bit;
}

static int get_bits(struct bits *b, int n) {
    int value = 0;
    for (int i = 0; i < n; i++) {
        int bit = get_bit(b);
        if (bit < 0) {
            return -1;
        }
        value |= bit << i;
    }
    return value;
}

/* A canonical Huffman code: how many codes of each length, and the symbols
 * in code order. */
struct huffman {
    uint16_t counts[16];
    uint16_t symbols[288];
};

static void build(struct huffman *h, const uint8_t *lengths, int n) {
    uint16_t offsets[16];
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; i++) {
        h->counts[lengths[i]]++;
    }
    h->counts[0] = 0;
    offsets[1] = 0;
    for (int i = 1; i < 15; i++) {
        offsets[i + 1] = offsets[i] + h->counts[i];
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i]) {
            h->symbols[offsets[lengths[i]]++] = (uint16_t)i;
        }
    }
}

static int decode(struct bits *b, const struct huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int length = 1; length < 16; length++) {
        int bit = get_bit(b);
        if (bit < 0) {
            return -1;
        }
        code |= bit;
        int count = h->counts[length];
        if (code - first < count) {
            return h->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                         15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                         67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                           17,   25,   33,   49,   65,   97,    129,   193,
                                           257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                           4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct output {
    uint8_t *data;
    size_t size, capacity;
};

static bool put(struct output *o, uint8_t byte) {
    if (o->size == o->capacity) {
        return false; /* The caller knows how much there should be. */
    }
    o->data[o->size++] = byte;
    return true;
}

static bool inflate_block(struct bits *b, struct output *o, const struct huffman *lengths,
                          const struct huffman *distances) {
    for (;;) {
        int symbol = decode(b, lengths);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 256) {
            if (!put(o, (uint8_t)symbol)) {
                return false;
            }
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) {
                return false;
            }
            int extra = get_bits(b, length_extra[symbol]);
            int d = decode(b, distances);
            if (extra < 0 || d < 0 || d >= 30) {
                return false;
            }
            int length = length_base[symbol] + extra;
            int dextra = get_bits(b, distance_extra[d]);
            if (dextra < 0) {
                return false;
            }
            size_t distance = distance_base[d] + (size_t)dextra;
            if (distance > o->size) {
                return false;
            }
            for (int i = 0; i < length; i++) {
                if (!put(o, o->data[o->size - distance])) {
                    return false;
                }
            }
        }
    }
}

/* Decodes a zlib stream into `out` (exactly `size` bytes expected). */
static bool inflate_zlib(const uint8_t *data, size_t size, uint8_t *out, size_t out_size) {
    if (size < 2 || (data[0] & 0x0f) != 8 || ((data[0] << 8) | data[1]) % 31) {
        return false;
    }
    struct bits b = {data + 2, size - 2, 0, 0, 0};
    struct output o = {out, 0, out_size};
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                      11, 4,  12, 3, 13, 2, 14, 1, 15};
    for (;;) {
        int last = get_bit(&b), type = get_bits(&b, 2);
        if (last < 0 || type < 0) {
            return false;
        }
        if (type == 0) { /* Stored. */
            b.count = 0;
            if (b.at + 4 > b.size) {
                return false;
            }
            size_t length = b.data[b.at] | b.data[b.at + 1] << 8;
            b.at += 4;
            if (b.at + length > b.size || o.size + length > o.capacity) {
                return false;
            }
            memcpy(o.data + o.size, b.data + b.at, length);
            o.size += length;
            b.at += length;
        } else if (type == 1 || type == 2) {
            static struct huffman lengths, distances;
            uint8_t code_lengths[320];
            if (type == 1) { /* The fixed codes. */
                int i = 0;
                for (; i < 144; i++) code_lengths[i] = 8;
                for (; i < 256; i++) code_lengths[i] = 9;
                for (; i < 280; i++) code_lengths[i] = 7;
                for (; i < 288; i++) code_lengths[i] = 8;
                build(&lengths, code_lengths, 288);
                for (i = 0; i < 30; i++) code_lengths[i] = 5;
                build(&distances, code_lengths, 30);
            } else {
                int nlen = get_bits(&b, 5) + 257, ndist = get_bits(&b, 5) + 1;
                int ncode = get_bits(&b, 4) + 4;
                if (nlen > 286 || ndist > 30 || ncode < 4) {
                    return false;
                }
                uint8_t cl[19] = {0};
                for (int i = 0; i < ncode; i++) {
                    int v = get_bits(&b, 3);
                    if (v < 0) {
                        return false;
                    }
                    cl[order[i]] = (uint8_t)v;
                }
                struct huffman code;
                build(&code, cl, 19);
                int n = 0;
                while (n < nlen + ndist) {
                    int symbol = decode(&b, &code);
                    if (symbol < 0) {
                        return false;
                    }
                    if (symbol < 16) {
                        code_lengths[n++] = (uint8_t)symbol;
                        continue;
                    }
                    int repeat, value = 0;
                    if (symbol == 16) {
                        if (n == 0) {
                            return false;
                        }
                        value = code_lengths[n - 1];
                        repeat = 3 + get_bits(&b, 2);
                    } else if (symbol == 17) {
                        repeat = 3 + get_bits(&b, 3);
                    } else {
                        repeat = 11 + get_bits(&b, 7);
                    }
                    if (repeat < 3 || n + repeat > nlen + ndist) {
                        return false;
                    }
                    while (repeat--) {
                        code_lengths[n++] = (uint8_t)value;
                    }
                }
                build(&lengths, code_lengths, nlen);
                build(&distances, code_lengths + nlen, ndist);
            }
            if (!inflate_block(&b, &o, &lengths, &distances)) {
                return false;
            }
        } else {
            return false;
        }
        if (last) {
            return o.size == o.capacity;
        }
    }
}

/* ---- Pixels ---- */

static uint32_t blend(uint32_t r, uint32_t g, uint32_t b, uint32_t a, uint32_t background) {
    if (background == VX_IMAGE_ALPHA) {
        return a << 24 | r << 16 | g << 8 | b;
    }
    uint32_t br = (background >> 16) & 0xff, bg = (background >> 8) & 0xff, bb = background & 0xff;
    r = (r * a + br * (255 - a)) / 255;
    g = (g * a + bg * (255 - a)) / 255;
    b = (b * a + bb * (255 - a)) / 255;
    return r << 16 | g << 8 | b;
}

static struct vx_image *new_image(int width, int height) {
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        return NULL;
    }
    struct vx_image *image = malloc(sizeof(*image));
    uint32_t *pixels = image ? malloc((size_t)width * height * 4) : NULL;
    if (!pixels) {
        free(image);
        return NULL;
    }
    image->surface = (struct vx_surface){pixels, width, height, width};
    return image;
}

void vx_image_free(struct vx_image *image) {
    if (image) {
        free(image->surface.pixels);
        free(image);
    }
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static struct vx_image *load_png(const uint8_t *data, size_t size, uint32_t background) {
    if (size < 33 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) {
        return NULL;
    }
    int width = 0, height = 0, depth = 0, color = -1;
    uint8_t palette[256 * 4];
    int palette_size = 0;
    memset(palette, 255, sizeof(palette));
    uint8_t *compressed = NULL;
    size_t compressed_size = 0;
    size_t at = 8;
    bool ok = true;
    while (ok && at + 12 <= size) {
        uint32_t length = be32(data + at);
        const uint8_t *type = data + at + 4, *body = data + at + 8;
        if (length > size - at - 12) {
            ok = false;
            break;
        }
        if (!memcmp(type, "IHDR", 4) && length >= 13) {
            width = (int)be32(body);
            height = (int)be32(body + 4);
            depth = body[8];
            color = body[9];
            if (body[10] || body[11] || body[12] || depth != 8) {
                ok = false; /* Compression/filter methods, interlacing, depths other than 8. */
            }
        } else if (!memcmp(type, "PLTE", 4)) {
            palette_size = (int)(length / 3 > 256 ? 256 : length / 3);
            for (int i = 0; i < palette_size; i++) {
                memcpy(palette + i * 4, body + i * 3, 3);
            }
        } else if (!memcmp(type, "tRNS", 4) && color == 3) {
            for (uint32_t i = 0; i < length && i < 256; i++) {
                palette[i * 4 + 3] = body[i];
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *more = realloc(compressed, compressed_size + length);
            if (!more) {
                ok = false;
                break;
            }
            compressed = more;
            memcpy(compressed + compressed_size, body, length);
            compressed_size += length;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + length;
    }
    int channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    struct vx_image *image = NULL;
    uint8_t *raw = NULL;
    size_t stride = (size_t)width * channels;
    if (ok && channels && compressed && (image = new_image(width, height)) &&
        (raw = malloc((stride + 1) * height)) &&
        inflate_zlib(compressed, compressed_size, raw, (stride + 1) * height)) {
        /* Undo the per-row filters, then convert. */
        for (int y = 0; y < height; y++) {
            uint8_t *row = raw + y * (stride + 1), filter = row[0];
            uint8_t *line = row + 1, *prior = y ? raw + (y - 1) * (stride + 1) + 1 : NULL;
            for (size_t x = 0; x < stride; x++) {
                int a = x >= (size_t)channels ? line[x - channels] : 0;
                int b = prior ? prior[x] : 0;
                int c = prior && x >= (size_t)channels ? prior[x - channels] : 0;
                int add = filter == 1 ? a : filter == 2 ? b : filter == 3 ? (a + b) / 2
                          : filter == 4 ? paeth(a, b, c) : 0;
                line[x] = (uint8_t)(line[x] + add);
            }
            uint32_t *out = image->surface.pixels + (long)y * width;
            for (int x = 0; x < width; x++) {
                const uint8_t *p = line + x * channels;
                switch (color) {
                case 0: out[x] = blend(p[0], p[0], p[0], 255, background); break;
                case 4: out[x] = blend(p[0], p[0], p[0], p[1], background); break;
                case 2: out[x] = blend(p[0], p[1], p[2], 255, background); break;
                case 6: out[x] = blend(p[0], p[1], p[2], p[3], background); break;
                case 3: {
                    const uint8_t *e = palette + p[0] * 4;
                    out[x] = blend(e[0], e[1], e[2], e[3], background);
                    break;
                }
                }
            }
        }
    } else {
        vx_image_free(image);
        image = NULL;
    }
    free(raw);
    free(compressed);
    return image;
}

static struct vx_image *load_bmp(const uint8_t *data, size_t size) {
    if (size < 54 || data[0] != 'B' || data[1] != 'M') {
        return NULL;
    }
    uint32_t offset = data[10] | data[11] << 8 | data[12] << 16 | (uint32_t)data[13] << 24;
    int32_t width = (int32_t)(data[18] | data[19] << 8 | data[20] << 16 | (uint32_t)data[21] << 24);
    int32_t height = (int32_t)(data[22] | data[23] << 8 | data[24] << 16 | (uint32_t)data[25] << 24);
    int bpp = data[28] | data[29] << 8;
    uint32_t compression = data[30] | data[31] << 8;
    bool bottom_up = height > 0;
    if (height < 0) {
        height = -height;
    }
    if ((bpp != 24 && bpp != 32) || (compression != 0 && compression != 3)) {
        return NULL;
    }
    size_t row = ((size_t)width * (bpp / 8) + 3) & ~(size_t)3;
    if (offset + row * height > size) {
        return NULL;
    }
    struct vx_image *image = new_image(width, height);
    if (!image) {
        return NULL;
    }
    for (int y = 0; y < height; y++) {
        const uint8_t *line = data + offset + row * (bottom_up ? height - 1 - y : y);
        uint32_t *out = image->surface.pixels + (long)y * width;
        for (int x = 0; x < width; x++) {
            const uint8_t *p = line + x * (bpp / 8);
            out[x] = (uint32_t)p[2] << 16 | p[1] << 8 | p[0];
        }
    }
    return image;
}

static const uint8_t *ppm_number(const uint8_t *p, const uint8_t *end, int *value) {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == '#')) {
        if (*p == '#') {
            while (p < end && *p != '\n') {
                p++;
            }
        } else {
            p++;
        }
    }
    *value = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        *value = *value * 10 + (*p++ - '0');
    }
    return p;
}

static struct vx_image *load_ppm(const uint8_t *data, size_t size) {
    if (size < 3 || data[0] != 'P' || data[1] != '6') {
        return NULL;
    }
    int width, height, max;
    const uint8_t *p = data + 2, *end = data + size;
    p = ppm_number(p, end, &width);
    p = ppm_number(p, end, &height);
    p = ppm_number(p, end, &max);
    p++; /* One whitespace character, then the pixels. */
    if (max != 255 || p + (size_t)width * height * 3 > end) {
        return NULL;
    }
    struct vx_image *image = new_image(width, height);
    if (!image) {
        return NULL;
    }
    for (long i = 0; i < (long)width * height; i++, p += 3) {
        image->surface.pixels[i] = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
    }
    return image;
}

struct vx_image *vx_image_decode(const void *data, size_t size, uint32_t background) {
    const uint8_t *bytes = data;
    struct vx_image *image = load_png(bytes, size, background);
    if (!image) {
        image = load_bmp(bytes, size);
    }
    if (!image) {
        image = load_ppm(bytes, size);
    }
    if (image && background == VX_IMAGE_ALPHA && !(bytes[0] == 0x89 && bytes[1] == 'P')) {
        struct vx_surface *s = &image->surface; /* BMP and PPM: opaque. */
        for (long i = 0; i < (long)s->width * s->height; i++) {
            s->pixels[i] |= VX_IMAGE_ALPHA;
        }
    }
    return image;
}

struct vx_image *vx_image_load(const char *path, uint32_t background) {
    struct vx_stat stat;
    if (vx_stat(path, &stat) || stat.size == 0 || stat.size > 64 * 1024 * 1024) {
        return NULL;
    }
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        return NULL;
    }
    uint8_t *data = malloc(stat.size);
    size_t got = 0;
    while (data && got < stat.size) {
        long n = vx_read(handle, data + got, stat.size - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    vx_close(handle);
    struct vx_image *image = data && got == stat.size ? vx_image_decode(data, got, background)
                                                      : NULL;
    free(data);
    return image;
}

/* Copies `from` into `to`'s rectangle, scaled (nearest pixel). */
void vx_blit_scaled(struct vx_surface *to, int x, int y, int width, int height,
                    const struct vx_surface *from) {
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; row++) {
        int ty = y + row;
        if (ty < 0 || ty >= to->height) {
            continue;
        }
        const uint32_t *source = from->pixels + (long)(row * from->height / height) * from->stride;
        uint32_t *line = to->pixels + (long)ty * to->stride;
        for (int col = 0; col < width; col++) {
            int tx = x + col;
            if (tx >= 0 && tx < to->width) {
                line[tx] = source[col * from->width / width];
            }
        }
    }
}

void vx_blit_alpha(struct vx_surface *to, int x, int y, int width, int height,
                   const struct vx_surface *from) {
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; row++) {
        int ty = y + row;
        if (ty < 0 || ty >= to->height) {
            continue;
        }
        /* The source pixels this one covers (at least one). */
        int y0 = row * from->height / height, y1 = (row + 1) * from->height / height;
        y1 = y1 > y0 ? y1 : y0 + 1;
        uint32_t *line = to->pixels + (long)ty * to->stride;
        for (int col = 0; col < width; col++) {
            int tx = x + col;
            if (tx < 0 || tx >= to->width) {
                continue;
            }
            int x0 = col * from->width / width, x1 = (col + 1) * from->width / width;
            x1 = x1 > x0 ? x1 : x0 + 1;
            /* Average, weighting colors by alpha. */
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint32_t *source = from->pixels + (long)sy * from->stride;
                for (int sx = x0; sx < x1; sx++, n++) {
                    uint32_t p = source[sx], pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 0xff) * pa;
                    g += ((p >> 8) & 0xff) * pa;
                    b += (p & 0xff) * pa;
                }
            }
            if (a == 0) {
                continue;
            }
            r /= a, g /= a, b /= a, a /= n;
            uint32_t d = line[tx];
            uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            line[tx] = ((r * a + dr * (255 - a)) / 255) << 16 | ((g * a + dg * (255 - a)) / 255) << 8 |
                       ((b * a + db * (255 - a)) / 255);
        }
    }
}
