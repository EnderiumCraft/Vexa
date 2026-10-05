#!/usr/bin/env python3
"""Draws the default wallpaper: glossy ribbons of light flowing over a deep
color, a glow behind them (in the spirit of Aqua and Aero). One for every
color scheme, rootfs/share/pictures/glass/<accent>-<light|dark>.png (the
accent's hue; deeper and dimmer for the dark theme), and glass.png, the teal
light one (the default colors), for Settings' pictures. Needs Pillow."""
import colorsys
import math
import os
from PIL import Image, ImageChops, ImageDraw, ImageFilter

# libvexa's accents (libvexa/src/theme.c): name and color.
ACCENTS = [("purple", 0xb07cff), ("blue", 0x4c8dff), ("teal", 0x2ec4b6), ("green", 0x3fbf6f),
           ("orange", 0xff8c3a), ("pink", 0xff5fa2), ("red", 0xf0524f), ("graphite", 0x8e8ea0)]

W, H = 1920, 1200
S = 2  # Drawn at twice the size, then made smaller: smooth edges.

# The colors are drawn for blue (its hue) and turned to each accent's.
BLUE_HUE = colorsys.rgb_to_hls(0x4c / 255, 0x8d / 255, 0xff / 255)[0]


def tint(rgb, accent, dark, lighten=1.0):
    """A blue of the picture in the accent's hue: as saturated as the accent
    is (graphite: nearly grey); darker for the dark theme."""
    h, l, s = colorsys.rgb_to_hls(*(c / 255 for c in rgb))
    ar, ag, ab = (accent >> 16) / 255, ((accent >> 8) & 255) / 255, (accent & 255) / 255
    ah, _, as_ = colorsys.rgb_to_hls(ar, ag, ab)
    # The accent's hue, about as far from it as the blue is from blue's (a
    # little closer: past warm hues that turns to another color).
    offset = (h - BLUE_HUE + 0.5) % 1.0 - 0.5
    away = abs((ah - BLUE_HUE + 0.5) % 1.0 - 0.5)
    h = (ah + offset * (1 - 0.5 * min(1.0, away / 0.15))) % 1.0
    s = s * min(1.0, as_ / 0.5)  # (Only graphite isn't colorful.)
    l = min(1.0, l * lighten)
    if dark:
        l *= 0.55
    out = colorsys.hls_to_rgb(h, l, s)
    # Greens, oranges and pinks look brighter than blue as light: about as
    # bright as the blue looks (most of the way there).
    def luma(c):
        return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
    blue = colorsys.hls_to_rgb(BLUE_HUE + offset, l, s)
    k = min(1.0, (luma(blue) / max(luma(out), 1e-6)) ** 0.6)
    return tuple(min(255, int(round(c * k * 255))) for c in out)


def sky_of(accent, dark):
    """Bright at the top to deep at the bottom (blue: to a navy)."""
    sky = Image.new("RGB", (W, H))
    draw = ImageDraw.Draw(sky)
    for y in range(H):
        t = y / (H - 1)
        rgb = (int(28 - 22 * t), int(96 - 70 * t), int(196 - 116 * t))
        draw.line([(0, y), (W, y)], fill=tint(rgb, accent, dark))
    return sky


# A soft glow, a little left of the middle.
glow = Image.new("L", (W, H), 0)
ImageDraw.Draw(glow).ellipse([W * 0.12, H * 0.18, W * 0.72, H * 0.86], fill=150)
glow = glow.filter(ImageFilter.GaussianBlur(220))


def curve(x, base, amplitude, phase, freq):
    return base + amplitude * math.sin(x / W * 2 * math.pi * freq + phase) \
        + amplitude * 0.35 * math.sin(x / W * 2 * math.pi * freq * 2.3 + phase * 1.7)


def ribbon(base, amplitude, phase, freq, width, twist, alpha):
    """A band between two curves; it narrows and widens as it flows. Lit
    along its top edge, fading down (glossy). Its masks: the band, the shine."""
    w, h = W * S, H * S
    mask = Image.new("L", (w, h), 0)
    shine = Image.new("L", (w, h), 0)
    top, bottom = [], []
    for i in range(0, W + 9, 6):
        x = i
        y = curve(x, base, amplitude, phase, freq)
        thick = width * (0.55 + 0.45 * math.sin(x / W * 2 * math.pi * twist + phase))
        top.append((x * S, y * S))
        bottom.append((x * S, (y + thick) * S))
    ImageDraw.Draw(mask).polygon(top + bottom[::-1], fill=255)
    # The shine: a thin bright line along the top edge.
    ImageDraw.Draw(shine).line(top, fill=255, width=3 * S)
    mask = mask.resize((W, H), Image.LANCZOS)
    shine = shine.resize((W, H), Image.LANCZOS).filter(ImageFilter.GaussianBlur(1.2))
    # Inside the band: brighter near its top edge (a vertical fade per column).
    fade = Image.new("L", (W, H), 0)
    fd = ImageDraw.Draw(fade)
    for i in range(0, W + 6, 6):
        y = curve(i, base, amplitude, phase, freq)
        thick = width * (0.55 + 0.45 * math.sin(i / W * 2 * math.pi * twist + phase))
        steps = max(int(thick), 1)
        for k in range(0, steps, 2):
            v = int(alpha * (1 - 0.75 * k / steps))
            fd.line([(i, y + k), (i + 6, y + k)], fill=v, width=2)
    band = ImageChops.multiply(mask, fade).filter(ImageFilter.GaussianBlur(1.5))
    return band, shine


# The ribbons (drawn once): masks and their colors (for blue).
RIBBONS = [(ribbon(700, 120, 0.4, 0.8, 260, 1.1, 120), (120, 210, 255)),
           (ribbon(610, 150, 1.6, 0.7, 170, 0.9, 95), (190, 235, 255)),
           (ribbon(820, 90, 2.9, 0.9, 120, 1.4, 110), (80, 170, 250)),
           (ribbon(520, 170, 4.2, 0.6, 90, 1.2, 70), (230, 245, 255))]
# A little light from above, over everything (the gloss).
top = Image.new("L", (W, H), 0)
ImageDraw.Draw(top).rectangle([0, 0, W, H * 0.42], fill=40)
top = top.filter(ImageFilter.GaussianBlur(120))
white = Image.new("RGB", (W, H), (255, 255, 255))


def scaled(mask, amount):
    return mask.point(lambda v: int(v * amount))


def wallpaper(accent, dark):
    # In the dark: a dimmer glow, ribbons and shine (light on a night sky).
    dim = 0.6 if dark else 1.0
    image = sky_of(accent, dark)
    image = Image.composite(Image.new("RGB", (W, H), tint((110, 200, 255), accent, dark)), image,
                            scaled(glow, dim))
    for (band, shine), rgb in RIBBONS:
        image = Image.composite(Image.new("RGB", (W, H), tint(rgb, accent, dark, 1.1 if dark else 1.0)),
                                image, scaled(band, 0.85 if dark else 1.0))
        image = Image.composite(white, image, scaled(shine, 0.55 * dim))
    return Image.composite(white, image, scaled(top, 0.5 if dark else 1.0))


os.makedirs("rootfs/share/pictures/glass", exist_ok=True)
for name, accent in ACCENTS:
    for dark in (False, True):
        image = wallpaper(accent, dark)
        image.save("rootfs/share/pictures/glass/%s-%s.png" % (name, "dark" if dark else "light"),
                   optimize=True)
        if name == "teal" and not dark:  # (The default colors.)
            image.save("rootfs/share/pictures/glass.png", optimize=True)
