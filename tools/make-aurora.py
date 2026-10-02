#!/usr/bin/env python3
"""Draws rootfs/share/pictures/aurora.png, the default wallpaper: two glowing
bands (green, then violet) waving across a night sky. Needs Pillow."""
import math
import random
from PIL import Image, ImageChops, ImageDraw, ImageFilter

W, H = 1920, 1200
sky = Image.new("RGB", (W, H))
draw = ImageDraw.Draw(sky)
for y in range(H):  # Deep blue at the top to a dark violet at the bottom.
    t = y / (H - 1)
    draw.line([(0, y), (W, y)], fill=(int(14 + 14 * t), int(14 + 2 * t), int(92 - 50 * t)))


def band(color, base, amplitude, phase, thickness, blur):
    layer = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(layer)
    points = [(x, base + amplitude * math.sin(x / W * 2 * math.pi * 0.9 + phase)) for x in range(0, W + 8, 8)]
    d.line(points, fill=color, width=thickness, joint="curve")
    return layer.filter(ImageFilter.GaussianBlur(blur))


def add(a, b):
    return ImageChops.screen(a, b)  # Light adds to light, as glows do.


green = band((24, 214, 160), 380, 170, 3.3, 150, 60)
violet = band((150, 60, 220), 690, 70, 3.6, 130, 70)
image = add(add(sky, green), violet)
# A few faint stars in the dark parts.
d = ImageDraw.Draw(image)
random.seed(7)
for _ in range(260):
    x, y = random.randrange(W), random.randrange(H)
    r, g, b = image.getpixel((x, y))
    if r + g + b < 160:
        v = random.randrange(70, 170)
        d.point((x, y), fill=(v, v, v + 20 if v < 235 else 255))
image.save("rootfs/share/pictures/aurora.png", optimize=True)
