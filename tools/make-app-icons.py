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


def audio_file():
    """A page with a note on it."""
    im, d = page()
    c = (110, 80, 220, 255)
    d.ellipse([15 * S, 30 * S, 23 * S, 37 * S], fill=c)
    d.rectangle([21 * S, 16 * S, 23 * S, 34 * S], fill=c)
    d.polygon([(21 * S, 16 * S), (31 * S, 19 * S), (31 * S, 23 * S), (23 * S, 20 * S)], fill=c)
    return im


def video_file():
    """A page with a film frame and a play triangle on it."""
    im, d = page()
    d.rectangle([12 * S, 17 * S, 34 * S, 35 * S], fill=(40, 40, 52, 255))
    d.polygon([(20 * S, 21 * S), (28 * S, 26 * S), (20 * S, 31 * S)], fill=(90, 160, 255, 255))
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


def activity():
    im = gradient_tile((40, 44, 56), (16, 18, 24))
    d = ImageDraw.Draw(im)
    pts = [(8, 30), (14, 30), (17, 20), (21, 36), (25, 14), (29, 32), (33, 26), (40, 26)]
    d.line([(x * S, y * S) for x, y in pts], fill=(90, 230, 140, 255), width=3 * S, joint="curve")
    return im


def calculator():
    im = gradient_tile((90, 90, 104), (40, 40, 50))
    d = ImageDraw.Draw(im)
    box(d, 9, 8, 39, 17, 2, (190, 230, 200, 255))
    colors = [(70, 70, 82), (70, 70, 82), (255, 149, 0)]
    for row in range(3):
        for col in range(3):
            x, y = 9 + col * 11, 20 + row * 8
            box(d, x, y, x + 8, y + 6, 2, colors[col] + (255,))
    return im


def calendar():
    im = gradient_tile((250, 250, 252), (220, 220, 228))
    d = ImageDraw.Draw(im)
    box(d, 3, 3, 45, 16, 9, (230, 70, 70, 255))
    d.rectangle([3 * S, 11 * S, 45 * S, 16 * S], fill=(230, 70, 70, 255))
    for row in range(3):
        for col in range(5):
            x, y = 9 + col * 6.5, 21 + row * 7
            d.rectangle([x * S, y * S, (x + 4) * S, (y + 4) * S], fill=(120, 120, 140, 255))
    d.rectangle([(9 + 2 * 6.5) * S, 28 * S, (13 + 2 * 6.5) * S, 32 * S], fill=(230, 70, 70, 255))
    return im


def notes():
    im = gradient_tile((255, 230, 120), (240, 200, 60))
    d = ImageDraw.Draw(im)
    for i in range(4):
        d.line([(10 * S, (17 + i * 7) * S), (38 * S, (17 + i * 7) * S)], fill=(170, 130, 30, 255),
               width=2 * S)
    d.rectangle([3 * S, 3 * S, 45 * S, 10 * S], fill=(225, 175, 40, 255))
    return im


def paint():
    im = gradient_tile((60, 120, 210), (30, 60, 140))
    d = ImageDraw.Draw(im)
    d.ellipse([7 * S, 9 * S, 41 * S, 39 * S], fill=(245, 236, 220, 255))
    for (x, y), c in zip([(14, 16), (22, 13), (30, 16), (33, 25)],
                         [(230, 60, 60), (250, 190, 40), (60, 180, 90), (60, 120, 230)]):
        d.ellipse([x * S, y * S, (x + 6) * S, (y + 6) * S], fill=c + (255,))
    d.ellipse([16 * S, 26 * S, 24 * S, 34 * S], fill=(60, 120, 210, 255))
    return im


def music():
    """A double note, white, on a glossy blue-violet tile."""
    im = gradient_tile((120, 160, 255), (110, 60, 210))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([5 * S, 4 * S, 43 * S, 22 * S], 8 * S, fill=(255, 255, 255, 55))
    white = (255, 255, 255, 255)
    d.ellipse([11 * S, 29 * S, 21 * S, 37 * S], fill=white)
    d.ellipse([27 * S, 26 * S, 37 * S, 34 * S], fill=white)
    d.rectangle([18 * S, 13 * S, 21 * S, 33 * S], fill=white)
    d.rectangle([34 * S, 10 * S, 37 * S, 30 * S], fill=white)
    d.polygon([(18 * S, 13 * S), (37 * S, 9 * S), (37 * S, 15 * S), (18 * S, 19 * S)], fill=white)
    return im


def videos():
    """A strip of film with a play triangle, on a dark glossy tile."""
    im = gradient_tile((80, 84, 100), (24, 26, 36))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([5 * S, 4 * S, 43 * S, 22 * S], 8 * S, fill=(255, 255, 255, 40))
    d.rectangle([8 * S, 12 * S, 40 * S, 36 * S], fill=(16, 16, 22, 255))
    for i in range(6):
        d.rectangle([(10 + i * 5) * S, 13 * S, (13 + i * 5) * S, 15 * S], fill=(220, 220, 230, 255))
        d.rectangle([(10 + i * 5) * S, 33 * S, (13 + i * 5) * S, 35 * S], fill=(220, 220, 230, 255))
    d.polygon([(20 * S, 18 * S), (31 * S, 24 * S), (20 * S, 30 * S)], fill=(90, 160, 255, 255))
    return im


def device_manager():
    """A chip on a board."""
    im = gradient_tile((120, 200, 170), (40, 130, 110))
    d = ImageDraw.Draw(im)
    pin = (230, 240, 235, 255)
    for i in range(4):
        x = 16 + i * 5
        d.rectangle([x * S, 10 * S, (x + 2) * S, 14 * S], fill=pin)
        d.rectangle([x * S, 34 * S, (x + 2) * S, 38 * S], fill=pin)
        d.rectangle([10 * S, x * S, 14 * S, (x + 2) * S], fill=pin)
        d.rectangle([34 * S, x * S, 38 * S, (x + 2) * S], fill=pin)
    box(d, 13, 13, 35, 35, 3, (35, 45, 50, 255))
    box(d, 19, 19, 29, 29, 2, (120, 200, 170, 255))
    return im


def installer():
    """A disk, with an arrow going into it."""
    im = gradient_tile((150, 160, 255), (80, 90, 210))
    d = ImageDraw.Draw(im)
    white = (255, 255, 255, 255)
    box(d, 10, 28, 38, 38, 3, (35, 40, 70, 255))
    d.ellipse([31 * S, 32 * S, 34 * S, 35 * S], fill=(120, 230, 140, 255))
    d.rectangle([21 * S, 8 * S, 27 * S, 18 * S], fill=white)
    d.polygon([(15 * S, 17 * S), (33 * S, 17 * S), (24 * S, 26 * S)], fill=white)
    return im


def doom():
    """A skull, in hellish light."""
    im = gradient_tile((200, 60, 30), (90, 15, 10))
    d = ImageDraw.Draw(im)
    bone = (240, 232, 214, 255)
    dark = (60, 10, 8, 255)
    d.ellipse([11 * S, 8 * S, 37 * S, 32 * S], fill=bone)
    d.rectangle([16 * S, 28 * S, 32 * S, 38 * S], fill=bone)
    d.ellipse([15 * S, 17 * S, 22 * S, 25 * S], fill=dark)
    d.ellipse([26 * S, 17 * S, 33 * S, 25 * S], fill=dark)
    d.polygon([(24 * S, 25 * S), (22 * S, 29 * S), (26 * S, 29 * S)], fill=dark)
    for x in (19, 23, 27):
        d.line([(x * S, 32 * S), (x * S, 38 * S)], fill=dark, width=S)
    return im


def netsurf():
    """A globe: the web browser."""
    im = gradient_tile((70, 190, 255), (20, 90, 200))
    d = ImageDraw.Draw(im)
    white = (255, 255, 255, 255)
    d.ellipse([10 * S, 10 * S, 38 * S, 38 * S], outline=white, width=3 * S)
    d.ellipse([18 * S, 10 * S, 30 * S, 38 * S], outline=white, width=2 * S)
    d.line([(24 * S, 10 * S), (24 * S, 38 * S)], fill=white, width=2 * S)
    d.line([(10 * S, 24 * S), (38 * S, 24 * S)], fill=white, width=2 * S)
    d.arc([12 * S, 4 * S, 36 * S, 20 * S], 30, 150, fill=white, width=2 * S)
    d.arc([12 * S, 28 * S, 36 * S, 44 * S], 210, 330, fill=white, width=2 * S)
    return im


def help_book():
    im = gradient_tile((80, 170, 255), (30, 100, 210))
    d = ImageDraw.Draw(im)
    d.ellipse([10 * S, 8 * S, 38 * S, 36 * S], outline=(255, 255, 255, 255), width=3 * S)
    d.arc([18 * S, 14 * S, 30 * S, 26 * S], 180, 90, fill=(255, 255, 255, 255), width=3 * S)
    d.line([(24 * S, 26 * S), (24 * S, 29 * S)], fill=(255, 255, 255, 255), width=3 * S)
    d.ellipse([22 * S, 31 * S, 26 * S, 35 * S], fill=(255, 255, 255, 255))
    return im


def hello_app():
    """The SDK's app template: a smile."""
    im = gradient_tile((255, 190, 90), (240, 120, 40))
    d = ImageDraw.Draw(im)
    white = (255, 255, 255, 255)
    d.ellipse([17 * S, 15 * S, 21 * S, 21 * S], fill=white)
    d.ellipse([27 * S, 15 * S, 31 * S, 21 * S], fill=white)
    d.arc([13 * S, 14 * S, 35 * S, 35 * S], 20, 160, fill=white, width=3 * S)
    return im


def sdl_app():
    """The SDL demo: overlapping colored squares."""
    im = gradient_tile((60, 70, 110), (25, 30, 55))
    d = ImageDraw.Draw(im)
    for x, y, color in ((10, 10, (240, 80, 90)), (18, 18, (90, 200, 120)), (26, 26, (80, 150, 255))):
        box(d, x, y, x + 13, y + 13, 3, color + (230,))
    return im


# Files' own pictures (Files.vxapp/Contents/Resources): kinds of files, places.
FILE_ICONS = {
    "folder": files, "document": document, "text": text_file, "image": image_file,
    "audio": audio_file, "video": video_file,
    "program": program, "device": device, "computer": computer, "disk": disk,
    "trash": trash, "apps": apps_folder, "pictures": pictures_folder,
}

ICONS = {
    "Terminal": terminal, "Files": files, "Editor": editor, "Viewer": viewer,
    "Settings": settings, "About": about, "XTerm": xterm,
    "Monitor": activity, "Calculator": calculator, "Calendar": calendar,
    "Notes": notes, "Paint": paint, "Help": help_book, "DeviceManager": device_manager,
    "Installer": installer, "Doom": doom, "NetSurf": netsurf, "Music": music, "Videos": videos,
}

if __name__ == "__main__":
    root = os.path.join(os.path.dirname(__file__), "..", "apps")
    for name, draw in ICONS.items():
        im = shadow(draw()).resize((SIZE, SIZE), Image.LANCZOS)
        out = os.path.join(root, name + ".vxapp", "Contents", "Resources", "icon.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        im.save(out, optimize=True)
        print(out)
    # The SDK's: the app template's icon, and the SDL demo's.
    sdk = os.path.join(os.path.dirname(__file__), "..", "sdk")
    for draw, out in ((hello_app, os.path.join(sdk, "template", "Resources", "icon.png")),
                      (sdl_app, os.path.join(sdk, "examples", "sdl-demo", "Resources", "icon.png"))):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        shadow(draw()).resize((SIZE, SIZE), Image.LANCZOS).save(out, optimize=True)
        print(out)
    for name, draw in FILE_ICONS.items():
        im = shadow(draw()).resize((SIZE, SIZE), Image.LANCZOS)
        out = os.path.join(root, "Files.vxapp", "Contents", "Resources", name + ".png")
        im.save(out, optimize=True)
        print(out)
