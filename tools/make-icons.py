#!/usr/bin/env python3
"""Makes Vexa's icons from the artwork in art/icons (big PNGs, glossy, in the
spirit of Aero), 96 by 96 (programs draw them smaller, averaging pixels):

- /share/icons/<name>.png: the whole set, for any program
- the apps' icons (apps/*.vxapp/Contents/Resources/icon.png)
- Files' icons for kinds of files and places (Files.vxapp's Resources; the
  desktop, the Installer and the Open and Save dialogs use them too)

A few are made of two: an audio file is a page with a note, the Pictures
folder a folder with a picture, XTerm the terminal with an X.

The PNGs it makes are kept in the repository; run this (it needs Pillow)
only to change them.
"""
import glob
import os
from PIL import Image, ImageDraw, ImageFilter

SIZE = 96
ART = "art/icons"
SHARE = "rootfs/share/icons"


def load(name):
    """An icon from the artwork: its picture, square (centered, a little
    room around it), still big."""
    path = glob.glob(os.path.join(ART, "*_%s.png" % name))[0]
    im = Image.open(path).convert("RGBA")
    im = im.crop(im.getbbox())
    side = int(max(im.size) * 1.06)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(im, ((side - im.width) // 2, (side - im.height) // 2), im)
    return square


def small(im, size=SIZE):
    """Made smaller with its colors weighted by how opaque they are (no dark
    edges where it fades out)."""
    return im.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")


def badge(base, mark, scale=0.55, edge=False):
    """`mark`, smaller, over the bottom right corner of `base` (with a white
    edge around it, so it stands out on the same color)."""
    out = base.copy()
    side = int(base.width * scale)
    m = small(mark, side)
    if edge:
        rim = m.split()[3].filter(ImageFilter.MaxFilter(2 * (side // 40) + 1))
        white = Image.new("RGBA", m.size, (255, 255, 255, 0))
        white.putalpha(rim.point(lambda v: v * 9 // 10))
        out.alpha_composite(white, (base.width - side, base.height - side))
    out.alpha_composite(m, (base.width - side, base.height - side))
    return out


def note():
    """The note from the music folder (the blue part of its right side)."""
    folder = load("music-folder")
    w, h = folder.size
    px = folder.load()
    xs, ys = [], []
    for y in range(h):
        for x in range(w // 2, w):
            r, g, b, a = px[x, y]
            if a > 200 and b > r + 70 and b > g + 20:
                xs.append(x)
                ys.append(y)
    box = (min(xs) - 4, min(ys) - 4, max(xs) + 5, max(ys) + 5)
    part = folder.crop(box)
    # Only the note: what isn't blue fades out.
    pp = part.load()
    for y in range(part.height):
        for x in range(part.width):
            r, g, b, a = pp[x, y]
            if not (b > r + 40 and b > g + 5) and not (r > 200 and g > 200 and b > 200 and a > 0
                                                       and b > r - 10):
                pp[x, y] = (r, g, b, 0)
    side = max(part.size)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(part, ((side - part.width) // 2, (side - part.height) // 2), part)
    return square


def x_mark():
    """A glossy orange disc with a white X (X11)."""
    s = 256
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([8, 8, s - 8, s - 8], fill=(150, 50, 10, 255))
    d.ellipse([16, 16, s - 16, s - 16], fill=(240, 110, 30, 255))
    d.ellipse([40, 22, s - 40, s // 2], fill=(255, 200, 150, 120))
    w = 30
    d.line([(78, 78), (s - 78, s - 78)], fill=(255, 255, 255, 255), width=w)
    d.line([(s - 78, 78), (78, s - 78)], fill=(255, 255, 255, 255), width=w)
    return im


# The set, by the names programs use (/share/icons/<name>.png).
SET = {
    "computer": "computer", "folder": "folder", "globe": "internet-globe",
    "compass": "compass", "network": "network", "trash": "trash-recycle",
    "control-panel": "control-panel", "document": "document", "notepad": "notepad",
    "palette": "paint-palette", "calculator": "calculator", "search": "search-magnifier",
    "pictures": "pictures", "music-folder": "music-folder", "video": "video-film",
    "disc": "cd-disc", "drive": "drive", "floppy": "floppy-disk", "printer": "printer",
    "users": "users", "user-folder": "user-folder", "my-network": "my-network",
    "settings": "settings-window", "terminal": "terminal", "calendar": "calendar",
    "text": "document-2", "film": "video-film-2", "help": "help", "store": "store",
    "usb": "usb-devices", "monitor": "system-monitor", "shield": "security-shield",
    "firewall": "firewall", "network-cable": "network-cable", "power": "power",
}

# Each app's icon (Doom keeps its own).
APPS = {
    "About": "computer", "Calculator": "calculator", "Calendar": "calendar",
    "DeviceManager": "usb", "Editor": "text", "Files": "folder", "Help": "help",
    "Installer": "disc", "Monitor": "monitor", "Music": "music-folder", "NetSurf": "globe",
    "Notes": "notepad", "Paint": "palette", "Settings": "control-panel", "Software": "store",
    "Terminal": "terminal", "Videos": "video", "Viewer": "pictures",
}

# Files' icons: kinds of files, and places.
FILES = {
    "computer": "computer", "folder": "folder", "device": "usb", "apps": "settings",
    "text": "text", "document": "document", "video": "film", "program": "settings",
    "disk": "drive", "image": "pictures", "trash": "trash", "home": "user-folder",
    "music": "music-folder",
}


def main():
    big = {name: load(art) for name, art in SET.items()}
    os.makedirs(SHARE, exist_ok=True)
    for name, im in big.items():
        small(im).save(os.path.join(SHARE, name + ".png"), optimize=True)
    for app, name in APPS.items():
        small(big[name]).save("apps/%s.vxapp/Contents/Resources/icon.png" % app, optimize=True)
    small(badge(big["terminal"], x_mark(), 0.5)).save(
        "apps/XTerm.vxapp/Contents/Resources/icon.png", optimize=True)
    files = "apps/Files.vxapp/Contents/Resources"
    for name, icon in FILES.items():
        small(big[icon]).save(os.path.join(files, name + ".png"), optimize=True)
    small(badge(big["document"], note(), 0.62, edge=True)).save(os.path.join(files, "audio.png"),
                                                     optimize=True)
    small(badge(big["folder"], big["pictures"], 0.62)).save(os.path.join(files, "pictures.png"),
                                                           optimize=True)


if __name__ == "__main__":
    main()
