#!/usr/bin/env python3
"""Draws Vane's icon: a glossy tile with a weather vane on it, in the look of Vexa's icon set.

The PNG is kept in the repository (apps/Vane.vxapp/Contents/Resources/icon.png); run this (it
needs Pillow) only to change it. It is drawn at 4 times its size, then made smaller.
"""
import math
import os
from PIL import Image, ImageDraw, ImageFilter

SIZE = 96
S = 4
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "apps", "Vane.vxapp",
                   "Contents", "Resources", "icon.png")


def rotate(points, cx, cy, degrees):
    a = math.radians(degrees)
    c, s = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in points]


def scale(points):
    return [(x * S, y * S) for x, y in points]


def main():
    size = SIZE * S
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))

    # The tile: a vertical gradient in a rounded square.
    top, bottom = (96, 214, 232), (16, 92, 148)
    fill = Image.new("RGBA", (size, size))
    fd = ImageDraw.Draw(fill)
    for y in range(size):
        t = y / (size - 1)
        fd.line([(0, y), (size, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([6 * S, 6 * S, 90 * S, 90 * S], 18 * S, fill=255)
    im.paste(fill, (0, 0), mask)

    # The shine on the top half.
    shine = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shine)
    sd.ellipse([-10 * S, -50 * S, 106 * S, 52 * S], fill=(255, 255, 255, 64))
    shine_mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(shine_mask).rounded_rectangle([6 * S, 6 * S, 90 * S, 90 * S], 18 * S, fill=255)
    clipped = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    clipped.paste(shine, (0, 0), shine_mask)
    im.alpha_composite(clipped)

    d = ImageDraw.Draw(im)
    # A faint ring, like a compass card.
    d.ellipse([20 * S, 20 * S, 76 * S, 76 * S], outline=(255, 255, 255, 90), width=2 * S)
    for k in range(4):
        a = math.radians(k * 90)
        x0, y0 = 48 + 28 * math.sin(a), 48 - 28 * math.cos(a)
        x1, y1 = 48 + 33 * math.sin(a), 48 - 33 * math.cos(a)
        d.line([(x0 * S, y0 * S), (x1 * S, y1 * S)], fill=(255, 255, 255, 150), width=2 * S)

    # The vane: a post, and an arrow with a tail, turned a little into the wind.
    d.rounded_rectangle([46.5 * S, 50 * S, 49.5 * S, 82 * S], 1 * S, fill=(255, 255, 255, 235))
    arrow = [(20, 46), (62, 46), (62, 39), (80, 48), (62, 57), (62, 50), (20, 50)]
    tail = [(20, 38), (34, 38), (28, 48), (34, 58), (20, 58), (24, 48)]
    arrow = rotate(arrow, 48, 48, -28)
    tail = rotate(tail, 48, 48, -28)

    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sd.polygon(scale([(x + 0.8, y + 1.6) for x, y in arrow]), fill=(0, 30, 60, 120))
    sd.polygon(scale([(x + 0.8, y + 1.6) for x, y in tail]), fill=(0, 30, 60, 120))
    shadow = shadow.filter(ImageFilter.GaussianBlur(1.6 * S))
    im.alpha_composite(shadow)

    d = ImageDraw.Draw(im)
    d.polygon(scale(tail), fill=(236, 246, 252, 255))
    d.polygon(scale(arrow), fill=(255, 255, 255, 255))
    d.ellipse([43 * S, 43 * S, 53 * S, 53 * S], fill=(255, 255, 255, 255), outline=(120, 190, 215, 255), width=1 * S)
    d.ellipse([46 * S, 46 * S, 50 * S, 50 * S], fill=(24, 110, 160, 255))

    # A soft edge at the bottom, like the other icons.
    edge = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle([6 * S, 6 * S, 90 * S, 90 * S], 18 * S, outline=(8, 52, 96, 150),
                                           width=1 * S)
    im.alpha_composite(edge)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    im.resize((SIZE, SIZE), Image.LANCZOS).save(OUT)
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
