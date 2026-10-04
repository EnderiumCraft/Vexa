#!/usr/bin/env python3
"""Draws rootfs/share/pictures/glass.png, the default wallpaper: glossy
ribbons of light flowing over a deep blue, a glow behind them (in the spirit
of Aqua and Aero). Needs Pillow."""
import math
from PIL import Image, ImageChops, ImageDraw, ImageFilter

W, H = 1920, 1200
S = 2  # Drawn at twice the size, then made smaller: smooth edges.

sky = Image.new("RGB", (W, H))
draw = ImageDraw.Draw(sky)
for y in range(H):  # A bright blue at the top to a deep navy at the bottom.
    t = y / (H - 1)
    draw.line([(0, y), (W, y)], fill=(int(28 - 22 * t), int(96 - 70 * t), int(196 - 116 * t)))

# A soft glow, a little left of the middle.
glow = Image.new("L", (W, H), 0)
ImageDraw.Draw(glow).ellipse([W * 0.12, H * 0.18, W * 0.72, H * 0.86], fill=150)
glow = glow.filter(ImageFilter.GaussianBlur(220))
sky = Image.composite(Image.new("RGB", (W, H), (110, 200, 255)), sky, glow)


def curve(x, base, amplitude, phase, freq):
    return base + amplitude * math.sin(x / W * 2 * math.pi * freq + phase) \
        + amplitude * 0.35 * math.sin(x / W * 2 * math.pi * freq * 2.3 + phase * 1.7)


def ribbon(image, base, amplitude, phase, freq, width, twist, color, alpha):
    """A band between two curves; it narrows and widens as it flows. Lit
    along its top edge, fading down (glossy)."""
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
    layer = Image.new("RGB", (W, H), color)
    image = Image.composite(layer, image, band)
    white = Image.new("RGB", (W, H), (255, 255, 255))
    return Image.composite(white, image, shine.point(lambda v: v * 0.55))


image = sky
image = ribbon(image, 700, 120, 0.4, 0.8, 260, 1.1, (120, 210, 255), 120)
image = ribbon(image, 610, 150, 1.6, 0.7, 170, 0.9, (190, 235, 255), 95)
image = ribbon(image, 820, 90, 2.9, 0.9, 120, 1.4, (80, 170, 250), 110)
image = ribbon(image, 520, 170, 4.2, 0.6, 90, 1.2, (230, 245, 255), 70)
# A little light from above, over everything (the gloss).
top = Image.new("L", (W, H), 0)
ImageDraw.Draw(top).rectangle([0, 0, W, H * 0.42], fill=40)
top = top.filter(ImageFilter.GaussianBlur(120))
image = Image.composite(Image.new("RGB", (W, H), (255, 255, 255)), image, top)
image.save("rootfs/share/pictures/glass.png", optimize=True)
