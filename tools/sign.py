#!/usr/bin/env python3
"""sign.py - send text or images to the ESP32 matrix sign over UDP.

Examples:
  sign.py text "{red}DISK 92%{/} on {cyan}nas01"
  sign.py text "{wave}Hello world" --scroll --speed 40
  sign.py text "{size:1}EAGLES\\n{green}24{/}-17"  (two lines: small label, big score)
  sign.py text "@bright=30"                  (options only: adjust, keep message)
  sign.py image photo.jpg                    (fit inside the panel, centered)
  sign.py image logo.png --fill --nearest    (crop to fill; pixel-art scaling)
  sign.py gif nyan.gif --loops 0             (stream an animated GIF; 0 = forever)
  sign.py clock                              (the sign keeps time itself; NTP + your timezone)
  sign.py clock --12h
  sign.py clock --template "{top:9}{font:bold12}{cyan}%k:%M"
  sign.py clear
  sign.py bright 40
  sign.py setup                              (reboot the sign into Wi-Fi setup mode)

Images need Pillow:  pip install pillow
"""
import argparse
import os
import socket
import struct
import sys
import time

DEFAULT_HOST = "sign.local"
DEFAULT_PORT = 4210
CHUNK = 1400                      # data bytes per UDP packet (stays under typical MTU)


# ---------------------------------------------------------------- transport

class Sign:
    def __init__(self, host, port, timeout=1.0):
        self.addr = (socket.gethostbyname(host), port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.frame_id = int(time.time()) & 0xFF

    def _reply(self):
        try:
            data, _ = self.sock.recvfrom(256)
            return data.decode(errors="replace")
        except socket.timeout:
            return None

    def send_text(self, payload, wait=True):
        self.sock.sendto(payload.encode("utf-8"), self.addr)
        return self._reply() if wait else None

    def send_image(self, img, wait=True, pace=0.002):
        """img: PIL RGB image no larger than the panel."""
        w, h = img.size
        data = img.tobytes()
        self.frame_id = (self.frame_id + 1) & 0xFF
        for off in range(0, len(data), CHUNK):
            header = struct.pack("<4sBBHHI", b"IMG1", 0, self.frame_id, w, h, off)
            self.sock.sendto(header + data[off:off + CHUNK], self.addr)
            time.sleep(pace)       # don't overrun the ESP32's small UDP queue
        return self._reply() if wait else None


# ---------------------------------------------------------------- images

def fit_image(img, panel_w, panel_h, fill=False, nearest=False):
    from PIL import Image, ImageOps
    resample = Image.Resampling.NEAREST if nearest else Image.Resampling.LANCZOS
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
        img = Image.alpha_composite(bg, img)
    img = img.convert("RGB")
    if fill:
        return ImageOps.fit(img, (panel_w, panel_h), method=resample)
    return ImageOps.contain(img, (panel_w, panel_h), method=resample)


def parse_panel(s):
    try:
        w, h = (int(v) for v in s.lower().split("x"))
        return w, h
    except ValueError:
        raise argparse.ArgumentTypeError("panel must look like 64x32")


# ---------------------------------------------------------------- commands

def cmd_text(sign, a):
    opts = []
    if a.scroll:
        opts.append("scroll")
    if a.static:
        opts.append("static")
    if a.once:
        opts.append("once")
    for name in ("font", "speed", "size", "gap", "color", "bright"):
        v = getattr(a, name)
        if v is not None:
            opts.append(f"{name}={v}")
    message = " ".join(a.message)
    if message == "-":
        message = sys.stdin.read().strip()
    payload = (f"@{' '.join(opts)}\n" if opts else "") + message
    return sign.send_text(payload)


def cmd_image(sign, a):
    from PIL import Image
    img = fit_image(Image.open(a.file), *a.panel, fill=a.fill, nearest=a.nearest)
    return sign.send_image(img)


def cmd_gif(sign, a):
    from PIL import Image, ImageSequence
    src = Image.open(a.file)
    frames = []
    for f in ImageSequence.Iterator(src):
        frames.append((fit_image(f.copy(), *a.panel, fill=a.fill, nearest=a.nearest),
                       max(f.info.get("duration", 100), 20) / 1000.0))
    print(f"{len(frames)} frames, Ctrl+C to stop")
    n = 0
    try:
        while a.loops == 0 or n < a.loops:
            for img, dur in frames:
                t0 = time.monotonic()
                sign.send_image(img, wait=False)
                time.sleep(max(0.0, dur - (time.monotonic() - t0)))
            n += 1
    except KeyboardInterrupt:
        pass
    return "done"


# ---------------------------------------------------------------- clock

DEFAULT_CLOCK = "{top:9}{font:bold12}{wave}%k:%M\\n{top:1}{#755}{size:1}%a %b %d"


def host_tz():
    """This computer's timezone as a POSIX TZ string (e.g. EST5EDT,M3.2.0,M11.1.0), or None.

    Linux/macOS zone files (TZif v2+) end with exactly that string on their last line.
    """
    tz = os.environ.get("TZ", "")
    if tz and "/" not in tz and not tz.startswith(":"):
        return tz
    for path in ("/etc/localtime",):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            continue
        if data[:4] == b"TZif" and data[4:5] in (b"2", b"3", b"4"):
            footer = data.rstrip(b"\n").rsplit(b"\n", 1)[-1].decode("ascii", "ignore").strip()
            if footer:
                return footer
    return None


def cmd_clock(sign, a):
    template = a.template or DEFAULT_CLOCK
    if a.twelve and not a.template:
        template = template.replace("%k", "%l")
    opts = ["clock", "static"]
    tz = a.tz or host_tz()
    if tz:
        opts.append(f"tz={tz}")
    else:
        print("(couldn't detect this computer's timezone; the sign will use its default - see --tz)")
    return sign.send_text(f"@{' '.join(opts)}\n{template}")


def main():
    p = argparse.ArgumentParser(description="Send text or images to the matrix sign.")
    p.add_argument("--host", default=DEFAULT_HOST, help=f"default {DEFAULT_HOST}")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--panel", type=parse_panel, default=(64, 32), help="WxH, default 64x32")
    sub = p.add_subparsers(dest="cmd", required=True)

    t = sub.add_parser("text", help="show a message ({tags} allowed; \\n = new line; '-' reads stdin)")
    t.add_argument("message", nargs="+")
    g = t.add_mutually_exclusive_group()
    g.add_argument("--scroll", action="store_true", help="always scroll")
    g.add_argument("--static", action="store_true", help="never scroll")
    t.add_argument("--once", action="store_true", help="scroll one time, then go blank (no repeat)")
    t.add_argument("--speed", type=float, help="scroll speed, px/sec")
    t.add_argument("--size", type=int, choices=[1, 2, 3, 4], help="scale (default 2 classic, 1 with --font)")
    t.add_argument("--font", help="classic tiny pico org sans9-24 bold9-24 mono9/12 serif9/12")
    t.add_argument("--gap", type=int, help="px between repeats")
    t.add_argument("--color", help="default color (name or #RRGGBB)")
    t.add_argument("--bright", type=int, help="0-255")

    for name, helptext in (("image", "show an image"), ("gif", "stream an animated GIF")):
        s = sub.add_parser(name, help=helptext)
        s.add_argument("file")
        s.add_argument("--fill", action="store_true", help="crop to fill instead of fitting")
        s.add_argument("--nearest", action="store_true", help="nearest-neighbor scaling (pixel art)")
        if name == "gif":
            s.add_argument("--loops", type=int, default=0, help="times to play, 0 = forever")

    c = sub.add_parser("clock", help="show a clock that the sign keeps itself (NTP)")
    c.add_argument("--template", help="message with date-style %%-codes, e.g. \"{font:bold12}%%k:%%M\"")
    c.add_argument("--12h", dest="twelve", action="store_true", help="12-hour time (default template only)")
    c.add_argument("--tz", help="POSIX timezone, e.g. EST5EDT,M3.2.0,M11.1.0 (default: this computer's)")

    sub.add_parser("setup", help="reboot the sign into Wi-Fi setup mode (to change networks)")
    sub.add_parser("clear", help="blank the display")
    b = sub.add_parser("bright", help="set brightness without changing the message")
    b.add_argument("level", type=int)

    a = p.parse_args()
    try:
        sign = Sign(a.host, a.port)
    except socket.gaierror:
        sys.exit(f"Can't resolve {a.host} - try --host <ip address>")

    if a.cmd == "text":
        r = cmd_text(sign, a)
    elif a.cmd == "image":
        r = cmd_image(sign, a)
    elif a.cmd == "gif":
        r = cmd_gif(sign, a)
    elif a.cmd == "clock":
        r = cmd_clock(sign, a)
    elif a.cmd == "setup":
        r = sign.send_text("@setup")
        if r and r.startswith("OK"):
            print("The sign is rebooting into setup mode - its panel shows the network to join.")
    elif a.cmd == "clear":
        r = sign.send_text("@clear")
    else:
        r = sign.send_text(f"@bright={a.level}")

    print(r if r else "(no reply - check host/port, or the sign may be busy)")
    sys.exit(0 if r and not r.startswith("ERR") else 1)


if __name__ == "__main__":
    main()
