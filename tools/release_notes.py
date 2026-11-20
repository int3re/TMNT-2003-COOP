"""A release's page for GitHub: the notes written by hand, plus the block tmntnet-launcher.exe reads.

    python tools/release_notes.py <notes.md> > <whole.md>
    gh release create v0.14.0 build/Release/tmntnet-launcher.exe build/Release/tmntnet-setup.exe --notes-file <whole.md> ...

The launcher learns about releases from github.com alone (src/launcher/update.h): the version
from where <repo>/releases/latest redirects, the rest from the release's page as the feed
<repo>/releases.atom gives it. The block at the page's end is that "rest": key=value lines in a
code block that starts with "tmntnet-release 1".

    version, date                 from src/common/version.h and today
    protocol                      kVersion of src/common/protocol.h
    protocol_since, title, note   dist/release.txt, written by hand for each release (the launcher
                                  carries that file too; its version and protocol are checked here)
    setup*, launcher*             the files' names, sizes and SHA-256 out of build/<Config>/

Both files must be the release's own: build first, upload exactly those.
"""
import argparse
import datetime
import hashlib
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8-sig") as f:
        return f.read()


def facts(config, name):
    path = os.path.join(ROOT, "build", config, name)
    if not os.path.isfile(path):
        sys.exit(f"build first: {path} is missing")
    data = open(path, "rb").read()
    return len(data), hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("notes", help="the release notes written by hand (markdown)")
    ap.add_argument("--config", default="Release")
    args = ap.parse_args()

    version = re.search(r'TMNTNET_VERSION\s+"([0-9.]+)"', read("src", "common", "version.h")).group(1)
    protocol = re.search(r"kVersion\s*=\s*(\d+)", read("src", "common", "protocol.h")).group(1)
    lines = ["tmntnet-release 1", f"version={version}", f"protocol={protocol}"]
    by_hand = {"protocol_since": None, "title": None, "note": []}
    for line in read("dist", "release.txt").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        if key == "note":
            by_hand["note"].append(value)
        elif key in by_hand:
            by_hand[key] = value
        elif key == "version" and value != version:
            sys.exit(f"dist/release.txt is about {value}, version.h says {version}: write this release's lines")
        elif key == "protocol" and value != protocol:
            sys.exit(f"dist/release.txt says protocol {value}, protocol.h says {protocol}")
    if not by_hand["title"] or not by_hand["note"]:
        sys.exit("dist/release.txt: a title and at least one note are needed")
    if len(by_hand["note"]) > 4:
        sys.exit("dist/release.txt: the launcher's sheet has room for four notes")
    if by_hand["protocol_since"]:
        lines.append("protocol_since=" + by_hand["protocol_since"])
    lines.append("date=" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"))
    size, sha = facts(args.config, "tmntnet-setup.exe")
    lines += ["setup=tmntnet-setup.exe", f"setup_size={size}", f"setup_sha256={sha}"]
    size, sha = facts(args.config, "tmntnet-launcher.exe")
    lines += ["launcher=tmntnet-launcher.exe", f"launcher_size={size}", f"launcher_sha256={sha}"]
    lines.append("title=" + by_hand["title"])
    lines += ["note=" + n for n in by_hand["note"]]

    notes = open(args.notes, encoding="utf-8-sig").read().rstrip()
    if "tmntnet-release " in notes:
        sys.exit("the notes already carry a block: give the hand-written text alone")
    out = notes + "\n\n<details>\n<summary>Данные для лаунчера</summary>\n\n```\n" + "\n".join(lines) + "\n```\n\n</details>\n"
    sys.stdout.buffer.write(out.encode("utf-8"))


if __name__ == "__main__":
    main()
