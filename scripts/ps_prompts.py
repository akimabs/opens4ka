#!/usr/bin/env python3
# Draws the PlayStation button prompts and writes them as texture patches (native/texture_pack.h) over the
# Xbox icons of each game's font atlas. Run with Pillow: python3 macos/scripts/ps_prompts.py
import struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parent.parent / "Resources" / "textures"
SS = 8  # supersampling

FACE = {  # DualShock face symbol colours
    "cross": (124, 178, 232),
    "circle": (255, 102, 102),
    "square": (232, 153, 208),
    "triangle": (64, 226, 160),
}
FONT = None
for f in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/System/Library/Fonts/Helvetica.ttc"):
    if Path(f).exists():
        FONT = f
        break


def disc(d, w, h, pill=False):
    # a dark glossy button: rim, body, a soft highlight on top
    r = h / 2 if pill else min(w, h) / 2
    box = (SS * 0.5, SS * 0.5, w * SS - SS * 0.5, h * SS - SS * 0.5)
    rad = r * SS - SS * 0.5
    d.rounded_rectangle(box, rad, fill=(190, 190, 194, 255))
    inner = (box[0] + SS, box[1] + SS, box[2] - SS, box[3] - SS)
    d.rounded_rectangle(inner, rad - SS, fill=(28, 28, 30, 255))
    iw, ih = inner[2] - inner[0], inner[3] - inner[1]
    hi = (inner[0] + iw * 0.18, inner[1] + ih * 0.06, inner[2] - iw * 0.18, inner[1] + ih * 0.42)
    d.ellipse(hi, fill=(58, 58, 62, 255))


def symbol(d, kind, cx, cy, s):
    c = FACE[kind] + (255,)
    lw = max(SS * 1.6, s * 0.28)
    if kind == "cross":
        d.line((cx - s, cy - s, cx + s, cy + s), fill=c, width=int(lw))
        d.line((cx - s, cy + s, cx + s, cy - s), fill=c, width=int(lw))
    elif kind == "circle":
        d.ellipse((cx - s, cy - s, cx + s, cy + s), outline=c, width=int(lw))
    elif kind == "square":
        d.rectangle((cx - s * 0.9, cy - s * 0.9, cx + s * 0.9, cy + s * 0.9), outline=c, width=int(lw))
    elif kind == "triangle":
        t = [(cx, cy - s * 1.05), (cx + s * 1.1, cy + s * 0.8), (cx - s * 1.1, cy + s * 0.8)]
        d.polygon(t, outline=c, width=int(lw))


def text(d, label, cx, cy, size):
    font = ImageFont.truetype(FONT, int(size))
    d.text((cx, cy), label, font=font, fill=(235, 235, 235, 255), anchor="mm")


def icon(w, h, kind):
    im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx, cy = w * SS / 2, h * SS / 2
    if kind in FACE:
        disc(d, w, h)
        symbol(d, kind, cx, cy, min(w, h) * SS * 0.24)
    elif " " in kind:  # a pill of two: "L2 R2"
        disc(d, w, h, pill=True)
        a, b = kind.split()
        text(d, a, w * SS * 0.27, cy, h * SS * 0.5)
        text(d, b, w * SS * 0.73, cy, h * SS * 0.5)
    elif w > h * 1.2:  # a trigger's tab
        disc(d, w, h, pill=True)
        text(d, kind, cx, cy + SS * 0.2, h * SS * 0.55)
    else:
        disc(d, w, h)
        text(d, kind, cx, cy + SS * 0.2, h * SS * 0.5)
    return im.resize((w, h), Image.LANCZOS)


def write(name, size, rects, flip=False):
    # flip: the game keeps its textures upside down
    out = bytearray(struct.pack("<III", size[0], size[1], len(rects)))
    for x, y, w, h, kind in rects:
        im = icon(w, h, kind)
        if flip:
            im = im.transpose(Image.FLIP_TOP_BOTTOM)
        out += struct.pack("<HHHH", x, y, w, h) + im.tobytes("raw", "BGRA")
    OUT.joinpath(name).parent.mkdir(parents=True, exist_ok=True)
    OUT.joinpath(name).write_bytes(out)
    print(name, len(rects), "icons")


# Mortal Kombat: Shaolin Monks, the menu font's atlas (256 x 512)
write("mk/32be4cd9a9da9004.patch", (256, 512), [
    (146, 230, 20, 20, "square"),    # X
    (167, 230, 20, 20, "triangle"),  # Y
    (188, 230, 20, 20, "cross"),     # A
    (209, 230, 20, 20, "circle"),    # B
    (167, 209, 20, 20, "L1"),        # White
    (230, 209, 20, 20, "R1"),        # Black
    (188, 209, 42, 20, "L2 R2"),     # the L R pill
    (0, 230, 21, 21, "L2"),          # L trigger
    (21, 230, 21, 21, "R2"),         # R trigger
])

# Mortal Kombat: Shaolin Monks, the in game HUD's atlas (256 x 256)
write("mk/51b96aae3fe30597.patch", (256, 256), [
    (56, 114, 32, 32, "triangle"),  # Y
    (88, 114, 32, 32, "square"),    # X
    (56, 146, 32, 32, "cross"),     # A
    (88, 146, 32, 32, "circle"),    # B
    (190, 177, 41, 30, "L2"),       # the L trigger's tab
])

# Digimon Rumble Arena 2 (Europe): each prompt its own 64 x 64 texture, stored upside down
write("digimon/f46101ef2928c3ef.patch", (64, 64), [(1, 1, 62, 62, "cross")], flip=True)    # A
write("digimon/2ec347be45a93742.patch", (64, 64), [(1, 1, 62, 62, "circle")], flip=True)   # B


# Need for Speed: Most Wanted draws its prompts as white outlines, one 32 x 32 (or 16 x 16) texture each
WHITE = (255, 255, 255, 255)


def outline(w, h, box, kind):
    im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, y0, x1, y1 = (v * SS for v in box)
    cx, cy, r = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2
    lw = int(SS * 2)
    if kind in ("L2", "R2"):  # the shoulder's square tag
        d.rectangle((x0, y0, x1, y1), outline=WHITE, width=int(SS * 1.5) if w < 32 else lw)
        font = ImageFont.truetype(FONT, int((y1 - y0) * 0.62))
        d.text((cx, cy + SS * 0.3), kind, font=font, fill=WHITE, anchor="mm")
    else:
        d.ellipse((x0 + lw / 2, y0 + lw / 2, x1 - lw / 2, y1 - lw / 2), outline=WHITE, width=lw)
        s = r * 0.42
        sw = int(SS * 1.8)
        if kind == "cross":
            d.line((cx - s, cy - s, cx + s, cy + s), fill=WHITE, width=sw)
            d.line((cx - s, cy + s, cx + s, cy - s), fill=WHITE, width=sw)
        elif kind == "circle":
            d.ellipse((cx - s, cy - s, cx + s, cy + s), outline=WHITE, width=sw)
        elif kind == "square":
            d.rectangle((cx - s * 0.9, cy - s * 0.9, cx + s * 0.9, cy + s * 0.9), outline=WHITE, width=sw)
        elif kind == "triangle":
            d.polygon([(cx, cy - s * 1.05), (cx + s * 1.1, cy + s * 0.8), (cx - s * 1.1, cy + s * 0.8)], outline=WHITE,
                      width=sw)
        elif kind == "select":  # the PS2 Select button: a flat bar
            d.rounded_rectangle((cx - s * 1.1, cy - s * 0.45, cx + s * 1.1, cy + s * 0.45), s * 0.45, fill=WHITE)
        else:  # L1, R1
            font = ImageFont.truetype(FONT, int(r * 0.95))
            d.text((cx, cy + SS * 0.3), kind, font=font, fill=WHITE, anchor="mm")
    return im.resize((w, h), Image.LANCZOS)


def write_whole(name, size, box, kind):
    w, h = size
    im = outline(w, h, box, kind)
    out = struct.pack("<III", w, h, 1) + struct.pack("<HHHH", 0, 0, w, h) + im.tobytes("raw", "BGRA")
    OUT.joinpath(name).parent.mkdir(parents=True, exist_ok=True)
    OUT.joinpath(name).write_bytes(out)
    print(name, kind)


for name, kind in [("0867c1c3713c4edb", "cross"), ("5ef1f9b3586470b5", "circle"), ("6f09e16bf5ef3839", "square"),
                   ("5e220c69c4c9d85e", "triangle")]:
    write_whole(f"nfs-mw/{name}.patch", (32, 32), (5, 5, 27, 27), kind)
write_whole("nfs-mw/e300943221cfa8ab.patch", (32, 32), (5, 5, 27, 27), "R1")       # Black
write_whole("nfs-mw/ed6046a8940bdf33.patch", (32, 32), (5, 5, 27, 27), "L1")       # White
write_whole("nfs-mw/556794d053fc305e.patch", (32, 32), (7, 7, 25, 25), "select")   # Back
write_whole("nfs-mw/7b6a205b189a44ab.patch", (32, 32), (5, 6, 26, 25), "L2")
write_whole("nfs-mw/813c8aa366b5793f.patch", (32, 32), (5, 6, 26, 25), "R2")


def tag(name, label):
    # the small 16 x 16 shoulder tags: white letters on a dark square
    im = Image.new("RGBA", (16 * SS, 16 * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((1 * SS, 2 * SS, 15 * SS, 15 * SS), fill=(16, 16, 16, 255), outline=(90, 90, 90, 255), width=SS)
    font = ImageFont.truetype(FONT, int(9 * SS))
    d.text((8 * SS, 8.7 * SS), label, font=font, fill=WHITE, anchor="mm")
    im = im.resize((16, 16), Image.LANCZOS)
    out = struct.pack("<III", 16, 16, 1) + struct.pack("<HHHH", 0, 0, 16, 16) + im.tobytes("raw", "BGRA")
    OUT.joinpath(name).write_bytes(out)
    print(name, label)


tag("nfs-mw/f94e20bba228f78a.patch", "L2")
tag("nfs-mw/05856ec5e569bc9c.patch", "R2")
