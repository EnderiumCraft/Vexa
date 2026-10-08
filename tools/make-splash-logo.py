#!/usr/bin/env python3
"""Makes the Vexa logo for the boot screen: kernel/src/dev/splash_logo.c, from
rootfs/share/logo.png (a little smaller, premultiplied so its edges stay clean).

The kernel has no files at that point, so the picture is compiled in as ARGB
pixels; a transparent pixel is written as 0 to keep the file small.

The file it makes is kept in the repository; run this (it needs Pillow) only to
change the logo.
"""
from PIL import Image

SOURCE = "rootfs/share/logo.png"
OUTPUT = "kernel/src/dev/splash_logo.c"
HEADER = "kernel/include/vexa/splash_logo.h"
WIDTH = 288


def main():
    im = Image.open(SOURCE).convert("RGBA")
    im = im.crop(im.getchannel("A").getbbox())
    height = round(im.height * WIDTH / im.width)
    im = im.convert("RGBa").resize((WIDTH, height), Image.LANCZOS).convert("RGBA")
    px = im.load()
    with open(OUTPUT, "w") as out:
        out.write("/* The Vexa logo for the boot screen: ARGB, %d by %d (made by\n"
                  " * tools/make-splash-logo.py from rootfs/share/logo.png; a 0 is a\n"
                  " * transparent pixel). */\n" % (WIDTH, height))
        out.write('#include <stdint.h>\n#include <vexa/splash_logo.h>\n\n')
        out.write("const uint32_t splash_logo[SPLASH_LOGO_HEIGHT][SPLASH_LOGO_WIDTH] = {\n")
        for y in range(height):
            out.write("    {")
            cells = []
            for x in range(WIDTH):
                r, g, b, a = px[x, y]
                cells.append("0" if a == 0 else "0x%02x%02x%02x%02x" % (a, r, g, b))
            out.write(",".join(cells))
            out.write("},\n")
        out.write("};\n")
    with open(HEADER, "w") as out:
        out.write("#ifndef VEXA_SPLASH_LOGO_H\n#define VEXA_SPLASH_LOGO_H\n\n#include <stdint.h>\n\n"
                  "/* The Vexa logo, for the boot screen (made by tools/make-splash-logo.py):\n"
                  " * ARGB. */\n#define SPLASH_LOGO_WIDTH %d\n#define SPLASH_LOGO_HEIGHT %d\n"
                  "extern const uint32_t splash_logo[SPLASH_LOGO_HEIGHT][SPLASH_LOGO_WIDTH];\n\n"
                  "#endif\n" % (WIDTH, height))
    print("wrote", OUTPUT, HEADER, WIDTH, "x", height)


if __name__ == "__main__":
    main()
