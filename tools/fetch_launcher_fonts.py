"""The launcher's three faces from Google Fonts, as small TTF files in third_party/fonts.

    python tools/fetch_launcher_fonts.py            (HTTPS_PROXY is honoured)

Google Fonts' css2 answers a client that names no modern browser with plain TTF
files, and `text=` cuts the file down to the letters asked for: Latin, Russian
and the punctuation the launcher writes. Letters outside it (a folder's name in
another script) come from Windows' own fonts: DirectWrite falls back by itself.
The faces are under the SIL Open Font License 1.1 (see third_party/fonts/README.md).
"""
import os
import re
import sys
import urllib.parse
import urllib.request

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "fonts")
AGENT = "tmntnet-build"  # (no browser named: the answer is TTF)

LETTERS = (
    "".join(chr(c) for c in range(0x20, 0x7F))
    + "".join(chr(c) for c in range(0x410, 0x450)) + "Ёё"
    + " «»—–…№·•’“”„←→↑↓×✓©"
)

FACES = [
    ("Dela Gothic One", "", [("400", "DelaGothicOne-Regular.ttf")]),
    ("Onest", ":wght@400;600;700", [("400", "Onest-Regular.ttf"), ("600", "Onest-SemiBold.ttf"), ("700", "Onest-Bold.ttf")]),
    ("JetBrains Mono", ":wght@500;700", [("500", "JetBrainsMono-Medium.ttf"), ("700", "JetBrainsMono-Bold.ttf")]),
]

LICENCES = [("delagothicone", "OFL-DelaGothicOne.txt"), ("onest", "OFL-Onest.txt"), ("jetbrainsmono", "OFL-JetBrainsMono.txt")]


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def main():
    os.makedirs(OUT, exist_ok=True)
    total = 0
    for family, axis, files in FACES:
        css = get("https://fonts.googleapis.com/css2?family=" + family.replace(" ", "+") + axis
                  + "&text=" + urllib.parse.quote(LETTERS)).decode("utf-8")
        blocks = re.findall(r"@font-face\s*\{(.*?)\}", css, re.S)
        found = {}
        for b in blocks:
            weight = re.search(r"font-weight:\s*(\d+)", b)
            src = re.search(r"url\((https://[^)]+)\)\s*format\('truetype'\)", b)
            if weight and src:
                found[weight.group(1)] = src.group(1)
        for weight, name in files:
            if weight not in found:
                sys.exit(f"{family} {weight}: no TTF in the answer:\n{css[:600]}")
            data = get(found[weight])
            if data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
                sys.exit(f"{name}: not a TrueType file")
            with open(os.path.join(OUT, name), "wb") as f:
                f.write(data)
            total += len(data)
            print(f"{name}: {len(data)} bytes")
    print(f"total {total} bytes")
    # Each face's licence as its authors publish it (github.com/google/fonts).
    for folder, name in LICENCES:
        text = get("https://raw.githubusercontent.com/google/fonts/main/ofl/" + folder + "/OFL.txt")
        if b"SIL OPEN FONT LICENSE" not in text.upper():
            sys.exit(f"{folder}: not the licence's text")
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(text)
        print(f"{name}: {len(text)} bytes")


if __name__ == "__main__":
    main()
