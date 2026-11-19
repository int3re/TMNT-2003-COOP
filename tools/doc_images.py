"""Pictures for docs/: the game's menus with the mod's rows outlined, and the 4:3 / wide comparison.

    python tools/doc_images.py

The sources are 1280x720 screenshots of lab runs ([debug] shots=..., see docs/BUILDING.md). They live
under runs/, which is not in the repository: after a menu has changed, take new screenshots and put
their paths (and the rows' rectangles) into the tables below. Needs Pillow and Windows' Segoe UI.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(ROOT, "runs")
OUT = os.path.join(ROOT, "docs", "images")

MARK = (255, 150, 30)  # the outline and the tag
TAG = "ДОБАВЛЕНО МОДОМ"
FONT = ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf", 19)
BIG = ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf", 26)

# picture, screenshot, the mod's rows (x0, y0, x1, y1), where the tag goes
MENUS = [
    ("menu-main.png", r"shots-081419\A-debug_menu_1500.png", [(243, 122, 432, 168)], "right"),
    ("menu-story.png", r"menus1-083106\debug_menu_4000.png", [(298, 262, 672, 374)], "right"),
    ("menu-graphics.png", r"menus1-083106\debug_menu_11800.png", [(280, 262, 1122, 622)], "top"),
    ("menu-sound.png", r"menus1-083106\debug_menu_16100.png", [(280, 328, 1008, 377), (280, 429, 800, 477)], "top"),
    ("menu-other.png", r"menus2-083409\debug_menu_12200.png", [(280, 324, 960, 528)], "right"),
    ("menu-controls.png", r"menus2-083409\debug_menu_19500.png", [(326, 347, 985, 396)], "top"),
]

# picture, screenshot: shown as they are
PLAIN = [
    ("menu-keys.png", r"menus2-083409\debug_menu_22600.png"),
    ("menu-options.png", r"menus1-083106\debug_menu_9300.png"),
    ("net-guest.png", r"shots2-084014\B-debug_menu_18000.png"),
    ("costume.jpg", r"shots-081419\A-debug_csel_24000.png"),
    ("level-map.jpg", r"shots2-084014\A-debug_csel_60000.png"),
    ("gameplay.jpg", r"shots2-084014\B-debug_csel_90000.png"),
]

# the same frame of the same run with [video] widescreen=0 and widescreen=1
NARROW = r"cmp-43-084419\debug_csel_56000.png"
WIDE = r"cmp-wide-084233\debug_csel_56000.png"


def load(rel):
    return Image.open(os.path.join(RUNS, rel)).convert("RGB")


def save(im, name):
    path = os.path.join(OUT, name)
    if name.endswith(".jpg"):
        im.convert("RGB").save(path, quality=90, optimize=True)
    else:
        im.convert("RGB").save(path, optimize=True)
    print(name, im.size, os.path.getsize(path))


def mark(im, boxes, tag_at):
    """Rounded outlines around the mod's rows, the tag at the first of them."""
    d = ImageDraw.Draw(im, "RGBA")
    for i, (x0, y0, x1, y1) in enumerate(boxes):
        d.rounded_rectangle((x0, y0, x1, y1), radius=10, outline=MARK + (255,), width=3)
        d.rounded_rectangle((x0 + 3, y0 + 3, x1 - 3, y1 - 3), radius=8, fill=MARK + (26,))
        if i:
            continue
        w = d.textlength(TAG, font=FONT)
        if tag_at == "right":  # beside the rows
            tx, ty = x1 + 24, (y0 + y1) // 2 - 12
        else:  # on the outline's top right corner
            tx, ty = x1 - w - 22, y0 - 15
        d.rounded_rectangle((tx - 10, ty - 3, tx + w + 10, ty + 27), radius=8, fill=MARK + (255,))
        d.text((tx, ty - 2), TAG, font=FONT, fill=(20, 20, 20, 255))
    return im


def compare():
    """The original 4:3 picture and the wide one side by side, the same height."""
    height, gap, bar = 540, 16, 46
    a, b = load(NARROW), load(WIDE)
    a = a.resize((int(a.width * height / a.height), height), Image.LANCZOS)
    b = b.resize((int(b.width * height / b.height), height), Image.LANCZOS)
    canvas = Image.new("RGB", (a.width + gap + b.width, height + bar), (18, 22, 20))
    canvas.paste(a, (0, bar))
    canvas.paste(b, (a.width + gap, bar))
    d = ImageDraw.Draw(canvas)
    d.text((12, 6), "Оригинал: 4:3", font=BIG, fill=(230, 230, 230))
    d.text((a.width + gap + 12, 6), "С модом: широкий экран, по бокам видно больше", font=BIG, fill=MARK)
    return canvas


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, shot, boxes, tag_at in MENUS:
        save(mark(load(shot), boxes, tag_at), name)
    for name, shot in PLAIN:
        save(load(shot), name)
    save(compare(), "widescreen-compare.jpg")
