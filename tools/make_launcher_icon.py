"""src/launcher/launcher.ico: the launcher's mark, a shell's plates in ooze green with ink lines.

    python tools/make_launcher_icon.py

The same drawing as in the window's bar (paint.cpp: caption), on a 28-unit square.
Needs Pillow. The .ico is committed: the build does not run this.
"""
import os

from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "launcher", "launcher.ico")
GREEN, DEEP, INK = (139, 232, 54, 255), (47, 125, 59, 255), (0, 0, 0, 255)
OUTER = [(14, 2), (24.4, 8), (24.4, 20), (14, 26), (3.6, 20), (3.6, 8)]
INNER = [(14, 8.5), (18.8, 11.25), (18.8, 16.75), (14, 19.5), (9.2, 16.75), (9.2, 11.25)]


def draw(size):
    k = 8  # drawn large, then made small: smooth edges
    s = size * k / 28.0
    img = Image.new("RGBA", (size * k, size * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def pts(ps):
        return [(x * s, y * s) for x, y in ps]

    def outline(ps, width):
        w = max(1, round(width * s))
        closed = pts(ps) + pts(ps[:1])
        d.line(closed, fill=INK, width=w, joint="curve")
        for x, y in pts(ps):  # (round corners)
            d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=INK)

    # (a small icon reads better with heavier lines)
    heavy = 2.0 if size >= 48 else 2.6 if size >= 24 else 3.2
    d.polygon(pts(OUTER), fill=GREEN)
    d.polygon(pts(INNER), fill=DEEP)
    for a, b in zip(INNER, OUTER):
        d.line(pts([a, b]), fill=INK, width=max(1, round(heavy * 0.8 * s)))
    outline(INNER, heavy * 0.8)
    outline(OUTER, heavy)
    return img.resize((size, size), Image.LANCZOS)


def main():
    sizes = [256, 64, 48, 32, 24, 16]
    images = [draw(n) for n in sizes]
    images[0].save(OUT, format="ICO", sizes=[(n, n) for n in sizes], append_images=images[1:])
    print(OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
