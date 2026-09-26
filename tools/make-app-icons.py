#!/usr/bin/env python3
"""Draws the apps' icons (apps/*.vxapp/Contents/Resources/icon.png).

The PNGs are kept in the repository; run this (it needs Pillow) only to
change them. Each is drawn at 4 times its size, then made smaller.
"""
import os
from PIL import Image, ImageDraw

SIZE = 48
S = 4  # Drawn at SIZE * S.


def canvas():
    return Image.new("RGBA", (SIZE * S, SIZE * S), (0, 0, 0, 0))


def box(d, x0, y0, x1, y1, radius, fill, outline=None, width=0):
    d.rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius * S, fill=fill,
                        outline=outline, width=width * S)


def gradient_tile(top, bottom, x0=3, y0=3, x1=45, y1=45, radius=9):
    """A rounded square with a vertical gradient."""
    im = canvas()
    fill = Image.new("RGBA", im.size)
    fd = ImageDraw.Draw(fill)
    for y in range(im.size[1]):
        t = y / (im.size[1] - 1)
        fd.line([(0, y), (im.size[0], y)],
                fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius * S, fill=255)
    im.paste(fill, (0, 0), mask)
    return im


def shadow(im):
    """A soft shadow under the shape."""
    alpha = im.split()[3]
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    sh.putalpha(alpha.point(lambda a: a * 90 // 255))
    out = canvas()
    out.paste(sh, (0, 2 * S), sh)
    out.alpha_composite(im)
    return out


def terminal():
    im = gradient_tile((70, 60, 110), (30, 24, 52))
    d = ImageDraw.Draw(im)
    box(d, 7, 11, 41, 41, 4, (14, 8, 24, 255))
    for i, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        d.ellipse([(9 + i * 4) * S, 6 * S, (12 + i * 4) * S, 9 * S], fill=c + (255,))
    d.line([(12 * S, 18 * S), (18 * S, 23 * S), (12 * S, 28 * S)], fill=(126, 231, 135, 255),
           width=3 * S, joint="curve")
    d.line([(21 * S, 29 * S), (30 * S, 29 * S)], fill=(126, 231, 135, 255), width=3 * S)
    return im


def files():
    im = canvas()
    d = ImageDraw.Draw(im)
    box(d, 4, 9, 22, 18, 3, (217, 160, 40, 255))
    box(d, 4, 13, 44, 41, 4, (242, 190, 70, 255))
    box(d, 4, 18, 44, 41, 4, (250, 208, 96, 255))
    d.line([(8 * S, 23 * S), (40 * S, 23 * S)], fill=(255, 226, 150, 255), width=S)
    return im


def editor():
    im = canvas()
    d = ImageDraw.Draw(im)
    box(d, 8, 3, 38, 45, 4, (246, 243, 252, 255), outline=(170, 160, 200, 255), width=1)
    for i in range(6):
        w = 20 if i % 3 != 2 else 13
        d.line([(13 * S, (12 + i * 5) * S), ((13 + w) * S, (12 + i * 5) * S)],
               fill=(138, 128, 163, 255), width=2 * S)
    # A pencil across the corner.
    d.polygon([(44 * S, 22 * S), (47 * S, 25 * S), (29 * S, 43 * S), (26 * S, 40 * S)],
              fill=(176, 124, 255, 255))
    d.polygon([(26 * S, 40 * S), (29 * S, 43 * S), (24 * S, 45 * S)], fill=(250, 220, 170, 255))
    d.polygon([(24.8 * S, 43.2 * S), (25.8 * S, 44.2 * S), (24 * S, 45 * S)], fill=(44, 29, 74, 255))
    return im


def viewer():
    im = canvas()
    d = ImageDraw.Draw(im)
    box(d, 3, 7, 45, 41, 4, (250, 250, 250, 255))
    inner = gradient_tile((80, 150, 240), (170, 210, 255), 6, 10, 42, 38, 2)
    d2 = ImageDraw.Draw(inner)
    d2.ellipse([31 * S, 13 * S, 37 * S, 19 * S], fill=(255, 220, 90, 255))
    d2.polygon([(6 * S, 38 * S), (18 * S, 20 * S), (28 * S, 34 * S), (33 * S, 27 * S), (42 * S, 38 * S)],
               fill=(60, 150, 80, 255))
    im.alpha_composite(inner)
    return im


def settings():
    im = gradient_tile((140, 150, 170), (80, 88, 104))
    d = ImageDraw.Draw(im)
    import math
    cx = cy = 24 * S
    teeth = []
    for i in range(16):
        a = i * math.pi / 8
        r = 16 * S if i % 2 == 0 else 12.5 * S
        for da in (-0.17, 0.17):
            teeth.append((cx + r * math.cos(a + da), cy + r * math.sin(a + da)))
    d.polygon(teeth, fill=(236, 238, 244, 255))
    d.ellipse([cx - 5 * S, cy - 5 * S, cx + 5 * S, cy + 5 * S], fill=(100, 108, 126, 255))
    return im


def about():
    im = canvas()
    d = ImageDraw.Draw(im)
    d.ellipse([4 * S, 4 * S, 44 * S, 44 * S], fill=(58, 110, 230, 255))
    d.ellipse([7 * S, 6 * S, 41 * S, 30 * S], fill=(96, 150, 255, 255))
    d.ellipse([4 * S, 4 * S, 44 * S, 44 * S], outline=(30, 60, 150, 255), width=S)
    d.ellipse([21.5 * S, 11 * S, 26.5 * S, 16 * S], fill=(255, 255, 255, 255))
    box(d, 21.5, 19, 26.5, 36, 2, (255, 255, 255, 255))
    return im


def xterm():
    im = gradient_tile((40, 40, 48), (12, 12, 16))
    d = ImageDraw.Draw(im)
    d.line([(12 * S, 12 * S), (36 * S, 36 * S)], fill=(235, 235, 240, 255), width=5 * S)
    d.line([(36 * S, 12 * S), (12 * S, 36 * S)], fill=(235, 235, 240, 255), width=3 * S)
    return im


def page(fold=(210, 204, 226)):
    """A sheet of paper with a folded corner."""
    im = canvas()
    d = ImageDraw.Draw(im)
    d.polygon([(9 * S, 3 * S), (30 * S, 3 * S), (39 * S, 12 * S), (39 * S, 45 * S), (9 * S, 45 * S)],
              fill=(246, 243, 252, 255), outline=(170, 160, 200, 255), width=S)
    d.polygon([(30 * S, 3 * S), (30 * S, 12 * S), (39 * S, 12 * S)], fill=fold + (255,),
              outline=(170, 160, 200, 255))
    return im, d


def document():
    return page()[0]


def text_file():
    im, d = page()
    for i in range(6):
        w = 20 if i % 3 != 2 else 12
        d.line([(14 * S, (18 + i * 4) * S), ((14 + w) * S, (18 + i * 4) * S)],
               fill=(138, 128, 163, 255), width=int(1.5 * S))
    return im


def image_file():
    im, d = page((150, 190, 240))
    d.rectangle([13 * S, 18 * S, 35 * S, 38 * S], fill=(120, 175, 245, 255))
    d.ellipse([27 * S, 20 * S, 32 * S, 25 * S], fill=(255, 220, 90, 255))
    d.polygon([(13 * S, 38 * S), (20 * S, 27 * S), (26 * S, 34 * S), (29 * S, 30 * S), (35 * S, 38 * S)],
              fill=(60, 150, 80, 255))
    return im


def program():
    im = gradient_tile((70, 60, 110), (30, 24, 52), 5, 8, 43, 40, 5)
    d = ImageDraw.Draw(im)
    d.rectangle([5 * S, 8 * S, 43 * S, 14 * S], fill=(110, 90, 170, 255))
    d.line([(12 * S, 21 * S), (18 * S, 26 * S), (12 * S, 31 * S)], fill=(126, 231, 135, 255),
           width=3 * S)
    d.line([(21 * S, 32 * S), (30 * S, 32 * S)], fill=(126, 231, 135, 255), width=3 * S)
    return im


def device():
    im = canvas()
    d = ImageDraw.Draw(im)
    box(d, 12, 12, 36, 36, 3, (90, 130, 220, 255))
    box(d, 17, 17, 31, 31, 2, (60, 90, 170, 255))
    for i in range(4):
        y = 15 + i * 6
        d.rectangle([7 * S, y * S, 12 * S, (y + 2) * S], fill=(170, 180, 200, 255))
        d.rectangle([36 * S, y * S, 41 * S, (y + 2) * S], fill=(170, 180, 200, 255))
    return im


def computer():
    im = canvas()
    d = ImageDraw.Draw(im)
    box(d, 4, 7, 44, 35, 3, (60, 64, 80, 255))
    inner = gradient_tile((110, 70, 200), (40, 20, 80), 7, 10, 41, 32, 1)
    im.alpha_composite(inner)
    d.polygon([(19 * S, 35 * S), (29 * S, 35 * S), (31 * S, 42 * S), (17 * S, 42 * S)],
              fill=(150, 155, 170, 255))
    d.rectangle([13 * S, 42 * S, 35 * S, 44 * S], fill=(120, 125, 140, 255))
    return im


def disk():
    im = canvas()
    d = ImageDraw.Draw(im)
    box(d, 4, 14, 44, 36, 4, (190, 195, 205, 255))
    box(d, 4, 28, 44, 36, 4, (150, 155, 168, 255))
    d.ellipse([36 * S, 30 * S, 40 * S, 34 * S], fill=(90, 220, 120, 255))
    return im


def trash():
    im = canvas()
    d = ImageDraw.Draw(im)
    d.polygon([(10 * S, 13 * S), (38 * S, 13 * S), (35 * S, 44 * S), (13 * S, 44 * S)],
              fill=(200, 205, 215, 255), outline=(130, 135, 150, 255), width=S)
    box(d, 7, 8, 41, 13, 2, (170, 175, 190, 255))
    box(d, 19, 4, 29, 8, 2, (170, 175, 190, 255))
    for x in (17, 24, 31):
        d.line([(x * S, 18 * S), (x * S, 40 * S)], fill=(140, 145, 160, 255), width=2 * S)
    return im


def apps_folder():
    im = files()
    d = ImageDraw.Draw(im)
    for i in range(2):
        for j in range(2):
            x, y = 16 + i * 9, 23 + j * 8
            box(d, x, y, x + 6, y + 6, 1.5, (255, 255, 255, 230))
    return im


def pictures_folder():
    im = files()
    d = ImageDraw.Draw(im)
    d.ellipse([28 * S, 24 * S, 33 * S, 29 * S], fill=(255, 255, 255, 230))
    d.polygon([(14 * S, 37 * S), (21 * S, 27 * S), (27 * S, 37 * S)], fill=(255, 255, 255, 230))
    return im


# Files' own pictures (Files.vxapp/Contents/Resources): kinds of files, places.
FILE_ICONS = {
    "folder": files, "document": document, "text": text_file, "image": image_file,
    "program": program, "device": device, "computer": computer, "disk": disk,
    "trash": trash, "apps": apps_folder, "pictures": pictures_folder,
}

ICONS = {
    "Terminal": terminal, "Files": files, "Editor": editor, "Viewer": viewer,
    "Settings": settings, "About": about, "XTerm": xterm,
}

if __name__ == "__main__":
    root = os.path.join(os.path.dirname(__file__), "..", "apps")
    for name, draw in ICONS.items():
        im = shadow(draw()).resize((SIZE, SIZE), Image.LANCZOS)
        out = os.path.join(root, name + ".vxapp", "Contents", "Resources", "icon.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        im.save(out, optimize=True)
        print(out)
    for name, draw in FILE_ICONS.items():
        im = shadow(draw()).resize((SIZE, SIZE), Image.LANCZOS)
        out = os.path.join(root, "Files.vxapp", "Contents", "Resources", name + ".png")
        im.save(out, optimize=True)
        print(out)
