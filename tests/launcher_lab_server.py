"""A stand-in for github.com in tmntnet-launcher's lab runs (tests/run_launcher_lab.ps1).

    python tests/launcher_lab_server.py --files <dir> --version 9.9.9 [--port 8765]

It answers the three things the launcher asks GitHub for, under /lab/repo:
    /lab/repo/releases/latest                     302 to .../releases/tag/v<version>
    /lab/repo/releases.atom                       the releases' feed, the launcher's block in the notes
    /lab/repo/releases/download/v<version>/<file> 302 to /cdn/<file>, as GitHub sends to its download host
    /cdn/<file>                                   the file out of --files

  --prose     the notes as written by hand before the launcher (no block; a table with the SHA-256)
  --bad-sha   the block names a SHA-256 the file does not have
  --slow N    the files go out at N KB/s (to see the progress and to cancel)
  --no-launcher  the block names no launcher file
  --log FILE  what was asked, into a file
"""
import argparse
import hashlib
import html
import http.server
import os
import sys
import time

p = argparse.ArgumentParser()
p.add_argument("--files", required=True)
p.add_argument("--version", required=True)
p.add_argument("--port", type=int, default=8765)
p.add_argument("--protocol", type=int, default=12)
p.add_argument("--since", default="0.13.8")
p.add_argument("--prose", action="store_true")
p.add_argument("--bad-sha", action="store_true")
p.add_argument("--no-launcher", action="store_true")
p.add_argument("--slow", type=int, default=0)
p.add_argument("--log", default="")
args = p.parse_args()
if args.log:  # (started without a console)
    sys.stdout = sys.stderr = open(args.log, "a", encoding="utf-8")

ROOT = "/lab/repo"
TAG = "v" + args.version


def facts(name):
    path = os.path.join(args.files, name)
    data = open(path, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    if args.bad_sha:
        sha = "0" * 64
    return len(data), sha


def notes_html():
    size, sha = facts("tmntnet-setup.exe")
    if args.prose:
        return ("<p>Проба лаунчера: <strong>релиз</strong> без блока, написан руками.</p>\n"
                "<ul>\n<li><strong>Первая строка.</strong> Длинное объяснение, которое в лист не идёт.</li>\n"
                "<li>Вторая строка без выделения. Её второе предложение.</li>\n</ul>\n"
                "<table><tbody><tr><td><code>tmntnet-setup.exe</code></td>\n<td><code>" + sha + "</code></td></tr></tbody></table>")
    lines = ["tmntnet-release 1", "version=" + args.version, "protocol=%d" % args.protocol, "protocol_since=" + args.since,
             "date=2026-11-21", "setup=tmntnet-setup.exe", "setup_size=%d" % size, "setup_sha256=" + sha]
    if not args.no_launcher and os.path.exists(os.path.join(args.files, "tmntnet-launcher.exe")):
        lsize, lsha = facts("tmntnet-launcher.exe")
        lines += ["launcher=tmntnet-launcher.exe", "launcher_size=%d" % lsize, "launcher_sha256=" + lsha]
    lines += ["title=Проба лаунчера: «кавычки» & амперсанд", "note=Первая строка заметок.", "note=Вторая строка, с <угловыми> скобками."]
    return ("<p>Проба лаунчера.</p>\n<details><summary>Данные для лаунчера</summary>\n"
            '<pre class="notranslate"><code class="notranslate">' + html.escape("\n".join(lines) + "\n") + "</code></pre>\n</details>")


def feed(host):
    base = "http://%s%s" % (host, ROOT)
    entry = ("  <entry>\n    <id>tag:lab,2008:Repository/1/%s</id>\n    <updated>2026-11-21T10:00:00Z</updated>\n"
             '    <link rel="alternate" type="text/html" href="%s/releases/tag/%s"/>\n    <title>tmntnet %s</title>\n'
             '    <content type="html">%s</content>\n  </entry>\n') % (TAG, base, TAG, args.version, html.escape(notes_html()))
    older = ("  <entry>\n    <id>tag:lab,2008:Repository/1/v0.0.1</id>\n    <updated>2020-01-01T00:00:00Z</updated>\n"
             '    <link rel="alternate" type="text/html" href="%s/releases/tag/v0.0.1"/>\n    <title>old</title>\n'
             '    <content type="html">&lt;p&gt;Старый релиз.&lt;/p&gt;</content>\n  </entry>\n') % base
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<feed xmlns="http://www.w3.org/2005/Atom">\n' + older + entry + "</feed>\n").encode("utf-8")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *a):
        sys.stdout.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % a))
        sys.stdout.flush()

    def redirect(self, to):
        self.send_response(302)
        self.send_header("Location", to)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == ROOT + "/releases/latest":
            return self.redirect(ROOT + "/releases/tag/" + TAG)
        if path == ROOT + "/releases.atom":
            body = feed(self.headers.get("Host", "127.0.0.1:%d" % args.port))
            self.send_response(200)
            self.send_header("Content-Type", "application/atom+xml; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        prefix = ROOT + "/releases/download/" + TAG + "/"
        if path.startswith(prefix):
            return self.redirect("/cdn/" + path[len(prefix):])
        if path.startswith("/cdn/"):
            name = os.path.basename(path)
            file = os.path.join(args.files, name)
            if not os.path.isfile(file):
                self.send_error(404)
                return
            data = open(file, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                if args.slow:
                    step = args.slow * 1024 // 10
                    for i in range(0, len(data), step):
                        self.wfile.write(data[i:i + step])
                        self.wfile.flush()
                        time.sleep(0.1)
                else:
                    self.wfile.write(data)
            except (BrokenPipeError, ConnectionError):
                pass  # (the launcher stopped the download)
            return
        self.send_error(404)


http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
