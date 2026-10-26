#!/bin/zsh
# Renders Resources/opens4ka-logo.svg into the app icon (Resources/AppIcon.icns) and the launcher's logo
# (Sources/OpenS4kaMac/Resources/opens4ka-logo.png). Needs Pillow; run after changing the logo.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 -I - "$ROOT_DIR/Resources/opens4ka-logo.svg" "$TMP/AppIcon.iconset" \
    "$ROOT_DIR/Sources/OpenS4kaMac/Resources/opens4ka-logo.png" <<'PY'
# The logo is pixel art: a rounded square and paths made of axis-aligned runs (M h v H z), drawn here directly so
# the corners stay transparent (Quick Look's renderer puts the art on white).
import os, re, sys
from PIL import Image, ImageDraw
svg, iconset, logo = sys.argv[1:]
text = open(svg).read()
view = float(re.search(r'viewBox="0 0 ([\d.]+)', text).group(1))
SS = 16  # supersampling for the rounded corners
size = int(view * SS)
art = Image.new("RGBA", (size, size), (0, 0, 0, 0))
d = ImageDraw.Draw(art)
for m in re.finditer(r'<rect([^>]*)/?>', text):
    a = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
    w, h, rx = float(a["width"]), float(a["height"]), float(a.get("rx", 0))
    d.rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), rx * SS, fill=a["fill"])
for m in re.finditer(r'<path([^>]*)/?>', text):
    a = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
    for sub in a["d"].split("M")[1:]:
        x = y = None
        xs, ys = [], []
        for cmd, nums in re.findall(r'([MhvHVz]?)\s*([-\d.\s]*)', "M" + sub):
            v = [float(n) for n in nums.split()]
            if cmd == "M": x, y = v[0], v[1]
            elif cmd == "h": x += v[0]
            elif cmd == "v": y += v[0]
            elif cmd == "H": x = v[0]
            elif cmd == "V": y = v[0]
            else: continue
            xs.append(x); ys.append(y)
        d.rectangle((min(xs) * SS, min(ys) * SS, max(xs) * SS - 1, max(ys) * SS - 1), fill=a["fill"])
art = art.resize((1024, 1024), Image.LANCZOS)
art.resize((512, 512), Image.LANCZOS).save(logo)
# macOS icons sit in a 1024 canvas with the artwork at 824, centred
canvas = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
canvas.alpha_composite(art.resize((824, 824), Image.LANCZOS), (100, 100))
os.makedirs(iconset)
for px in (16, 32, 128, 256, 512):
    for scale in (1, 2):
        name = f"icon_{px}x{px}{'@2x' if scale == 2 else ''}.png"
        canvas.resize((px * scale, px * scale), Image.LANCZOS).save(os.path.join(iconset, name))
PY
iconutil -c icns "$TMP/AppIcon.iconset" -o "$ROOT_DIR/Resources/AppIcon.icns"
echo "Icon: Resources/AppIcon.icns"
