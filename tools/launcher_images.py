"""docs/images/launcher*.png: the launcher's pictures for the README and the docs, drawn by the launcher itself.

    python tools/launcher_images.py --was 0.13.9        (after a build; needs Pillow for the last picture)

The launcher renders canned states into PNG files without showing a window (/shot). The pictures
are about the release being made: its version and notes (dist/release.txt) stand as the latest on
GitHub, --was as the version found in the game folder. Run it again for every release.
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "docs", "images")

# (state, file, scale)
SHOTS = [
    ("update", "launcher-hero.png", 1.5),         # the README's first picture, sharp on dense screens
    ("update", "launcher.png", 1),
    ("install", "launcher-install.png", 1),
    ("bundle", "launcher-first-install.png", 1),
    ("downloading", "launcher-download.png", 1),
    ("ready", "launcher-ready.png", 1),
    ("settings", "launcher-settings.png", 1),
    ("graphics", "launcher-graphics.png", 1),     # the game's settings on the launcher's pages
    ("sound", "launcher-sound.png", 1),
    ("controls", "launcher-controls.png", 1),
    ("selfupdate", "launcher-selfupdate.png", 1),  # at its start: a newer launcher is being taken
]


def shot(exe, state, file, scale, was, accent=None):
    args = [exe, "/shot", file, "/state", state, "/scale", str(scale), "/was", was]
    if accent is not None:
        args += ["/accent", str(accent)]
    if subprocess.run(args).returncode != 0 or not os.path.isfile(file):
        sys.exit(f"{state}: the launcher drew nothing ({file})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="Release")
    ap.add_argument("--was", required=True, help="the version before this release, e.g. 0.13.9")
    args = ap.parse_args()
    exe = os.path.join(ROOT, "build", args.config, "tmntnet-launcher.exe")
    if not os.path.isfile(exe):
        sys.exit(f"build first: {exe} is missing")
    for state, name, scale in SHOTS:
        shot(exe, state, os.path.join(OUT, name), scale, args.was)
        print(name)

    # The four headband colours side by side.
    from PIL import Image
    gap, w, h = 16, 480, 300
    sheet = Image.new("RGBA", (2 * w + gap, 2 * h + gap), (0, 0, 0, 0))
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(4):
            file = os.path.join(tmp, f"accent{i}.png")
            shot(exe, "update", file, 0.5, args.was, accent=i)
            sheet.paste(Image.open(file).convert("RGBA"), ((i % 2) * (w + gap), (i // 2) * (h + gap)))
    sheet.save(os.path.join(OUT, "launcher-accents.png"), optimize=True)
    print("launcher-accents.png")


if __name__ == "__main__":
    main()
