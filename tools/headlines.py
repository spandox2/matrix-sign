#!/usr/bin/env python3
"""headlines.py - scroll RSS/Atom headlines across the matrix sign, one at a time.

Each headline is shown once (fully scrolled across), then the next one is sent.
The feed is re-read every --refresh seconds.

Layout (64x32): a small static label on top, the headline scrolling below it.
    ARS 3/20
    <headline scrolling at size 2>

Examples:
  headlines.py                                   # Ars Technica, loops forever
  headlines.py --once                            # one pass through the headlines, then exit
  headlines.py --feed https://hnrss.org/frontpage --label HN --color amber
  headlines.py --host 192.168.68.112 --speed 45
  headlines.py --font sans9 --label-font tiny        # smooth font, more of the headline visible

Needs sign.py in the same folder. Standard library only (no extra packages).
"""
import argparse
import html
import os
import re
import sys
import time
import unicodedata
import urllib.request
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sign import Sign, DEFAULT_HOST, DEFAULT_PORT   # noqa: E402

DEFAULT_FEED = "https://feeds.arstechnica.com/arstechnica/index"
PANEL_W = 64
CHAR_W = 6            # font cell width at size 1
MAX_CHARS = 200       # keep well under the sign's 256-character limit

# Characters the sign's font can't show, mapped to plain equivalents
REPLACE = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'",
    "“": '"', "”": '"', "„": '"',
    "–": "-", "—": " - ", "−": "-",
    "…": "...", " ": " ", "•": "*",
}


def clean(text):
    """Turn a feed title into plain ASCII the sign can display."""
    text = html.unescape(text or "")
    text = re.sub(r"<[^>]+>", "", text)                 # stray HTML tags
    for k, v in REPLACE.items():
        text = text.replace(k, v)
    text = text.replace("°", "\x00DEG\x00")        # keep the degree sign
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    text = text.replace("\x00DEG\x00", "°")
    text = " ".join(text.split())                       # collapse whitespace/newlines
    if len(text) > MAX_CHARS:
        text = text[:MAX_CHARS - 3].rstrip() + "..."
    return text


def escape(text):
    """Escape characters the sign treats specially ({ tags, \\n, \\\\)."""
    return text.replace("\\", "\\\\").replace("{", "{{")


def fetch_titles(url, limit):
    req = urllib.request.Request(url, headers={"User-Agent": "matrix-sign-headlines/1.0"})
    with urllib.request.urlopen(req, timeout=15) as r:
        root = ET.fromstring(r.read())
    titles = []
    # RSS: <item><title>; Atom: <entry><title> (namespaced)
    for el in root.iter():
        tag = el.tag.rsplit("}", 1)[-1]
        if tag in ("item", "entry"):
            for child in el:
                if child.tag.rsplit("}", 1)[-1] == "title":
                    t = clean("".join(child.itertext()))
                    if t:
                        titles.append(t)
                    break
        if len(titles) >= limit:
            break
    return titles


def scroll_seconds(width, speed):
    """Time for one full pass: text enters from the right and fully exits left."""
    return (width + PANEL_W) / speed + 0.3


def reported_width(reply):
    """The sign answers "OK w=N" (newer firmware). Returns N, or None."""
    m = re.match(r"OK w=(\d+)", reply or "")
    return int(m.group(1)) if m else None


def main():
    p = argparse.ArgumentParser(description="Scroll feed headlines on the matrix sign.")
    p.add_argument("--feed", default=DEFAULT_FEED, help="RSS or Atom URL")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--label", default="ARS", help="short label for the top line (keep it ~5 chars)")
    p.add_argument("--color", default="orange", help="label color (name or #RRGGBB)")
    p.add_argument("--text-color", default="white", help="headline color")
    p.add_argument("--font", help="headline font, e.g. sans9 bold9 serif9 (default: classic)")
    p.add_argument("--label-font", default="classic", help="label font, e.g. tiny (default: classic)")
    p.add_argument("--size", type=int, choices=[1, 2, 3],
                   help="headline scale (default 2 for classic, 1 with --font)")
    p.add_argument("--label-top", type=int, default=1, help="row for the top of the label's capitals (default 1)")
    p.add_argument("--headline-top", type=int, default=12, help="row for the top of the headline's capitals (default 12)")
    p.add_argument("--speed", type=float, default=40, help="scroll speed, px/sec")
    p.add_argument("--max", type=int, default=20, help="max headlines per pass")
    p.add_argument("--refresh", type=int, default=900, help="re-read the feed every N seconds")
    p.add_argument("--once", action="store_true", help="show each headline once, then exit")
    a = p.parse_args()

    try:
        sign = Sign(a.host, a.port)
    except OSError:
        sys.exit(f"Can't resolve {a.host} - try --host <ip address>")

    if a.font:
        headline_style = f"{{font:{a.font}}}" + (f"{{size:{a.size}}}" if a.size else "")
    else:
        headline_style = f"{{size:{a.size or 2}}}"

    titles, fetched = [], 0.0
    try:
        while True:
            if not titles or time.time() - fetched > a.refresh:
                try:
                    titles = fetch_titles(a.feed, a.max)
                    fetched = time.time()
                    print(f"{len(titles)} headlines from {a.feed}")
                except Exception as e:                      # network/XML trouble: retry later
                    print(f"feed error: {e}", file=sys.stderr)
                    if not titles:
                        time.sleep(60)
                        continue
            for i, title in enumerate(titles, 1):
                label = f"{a.label} {i}/{len(titles)}"
                msg = (f"@once gap={PANEL_W} speed={a.speed:g}\n"   # gap: no repeat even on old firmware
                       f"{{top:{a.label_top}}}{{font:{a.label_font}}}{{size:1}}{{{a.color}}}{escape(label)}\n"
                       f"{{top:{a.headline_top}}}{headline_style}{{{a.text_color}}}{escape(title)}")
                reply = sign.send_text(msg)
                ok = bool(reply) and reply.startswith("OK")
                print(f"[{i}/{len(titles)}] {title}" + ("" if ok else f"   ({reply})"))
                width = reported_width(reply) or len(title) * CHAR_W * (a.size or 2)   # estimate on old firmware
                time.sleep(scroll_seconds(width, a.speed))
                if time.time() - fetched > a.refresh:
                    break                                   # refresh mid-pass
            if a.once:
                break
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
