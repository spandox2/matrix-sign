# MatrixSign — `sign.py` reference

`sign.py` sends text, images and animated GIFs to the ESP32-S3 matrix sign over UDP.
The sign always shows the **last thing it received**.

```
sign.py [--host HOST] [--port PORT] [--panel WxH] <command> [options]
```

Requirements: Python 3. Images and GIFs also need Pillow
(`sudo apt install python3-pil`, or `pip install pillow`).

---

## Wi-Fi setup

There's no Wi-Fi password in the code. The sign stores its network in flash and sets itself up from your phone.

1. **First power-up** (or when the saved network can't be found) the sign starts **setup mode**. The panel shows:
   - its own network name, `MatrixSign-XXXX`
   - a password (`pw ########`, new random digits each time)
   - the setup address, `192.168.4.1`
2. Join that network on your phone. The setup page opens by itself on most phones; if not, browse to `http://192.168.4.1`.
3. Pick your network from the list (strongest first) or type its name. Enter the password and, optionally,
   a **sign name**: the hostname, default `sign`, so the sign answers as `sign.local`.
4. Tap **Save & connect**. The sign restarts, joins your Wi-Fi and scrolls its address.

If the network is wrong or unreachable, the sign comes back to setup mode on its own.

| Situation | What happens |
|---|---|
| Saved network not found at power-up (e.g. the router is still booting after a power cut) | Setup mode starts, **and** the sign retries the saved network every minute. The retry only runs while no phone is connected to the setup network. As soon as the router answers, the sign goes back to normal. |
| `sign.py setup` (or a raw `@setup` packet) | Reboots into setup mode to change networks. If nobody finishes setup within 10 minutes, it returns to the saved network. |
| Hold the DevKit's **BOOT** button for 5 seconds | Forgets the saved network and restarts into setup mode. V2 adapter only: on V1.x, GPIO0 drives the panel. |

The settings (network, password, sign name) survive reflashing the sketch, unless you turn on
**Tools → Erase All Flash Before Sketch Upload**. To wipe them otherwise, use the BOOT button.

---

## Global options

These go **before** the command.

| Option | Default | Meaning |
|---|---|---|
| `--host HOST` | `sign.local` | Sign hostname or IP address |
| `--port PORT` | `4210` | UDP port |
| `--panel WxH` | `64x32` | Panel size, used to resize images and GIFs |

---

## Commands

### `text` — show a message

```
sign.py text [options] "message"
sign.py text [options] -          # read the message from stdin
```

| Option | Values | Default | Meaning |
|---|---|---|---|
| `--size N` | 1–4 | 2 (1 with `--font`) | Text size / scale (see table below) |
| `--font NAME` | see **Fonts** | classic | Font for every line |
| `--scroll` | | | Always scroll |
| `--static` | | | Never scroll (text that's too wide gets cut off on the right) |
| `--once` | | | Scrolling lines pass through **one time**, then go blank (no repeat). Static lines stay. |
| *(neither)* | | auto | Scroll only if the text is wider than the panel |
| `--speed N` | 1–500 | 30 | Scroll speed in pixels/second |
| `--gap N` | 0–512 | 24 | Blank pixels between repeats while scrolling |
| `--color X` | name or `#RRGGBB` | white | Color for text without a tag |
| `--bright N` | 0–255 | 80 | Panel brightness. Stays set until changed. |

`--scroll` and `--static` can't be used together.

#### Text sizes

The built-in font is 5×7 pixels with 1 pixel of spacing, scaled up in whole steps.
Sizes bigger than the panel height allows are reduced automatically.

| `--size` | Character size (px) | Characters across 64 px | Fits a 64×32 panel? |
|---|---|---|---|
| 1 | 6×8 | ~10 | yes |
| 2 | 12×16 | 5 | yes |
| 3 | 18×24 | 3 | yes |
| 4 | 24×32 | 2 | yes (full height) |

Messages can be up to 256 characters. See **Multi-line text** below for more than one line.

### `image` — show a picture

```
sign.py image [--fill] [--nearest] FILE
```

| Option | Meaning |
|---|---|
| *(default)* | Fit the whole image inside the panel, centered, with black borders as needed |
| `--fill` | Crop the image to fill the whole panel |
| `--nearest` | Sharp nearest-neighbor scaling (good for pixel art and icons) |

Any format Pillow can open works (PNG, JPG, GIF, BMP, WebP, …). Transparent areas show as black.

### `gif` — play an animated GIF

```
sign.py gif [--fill] [--nearest] [--loops N] FILE
```

| Option | Default | Meaning |
|---|---|---|
| `--fill` / `--nearest` | | Same as `image` |
| `--loops N` | 0 | How many times to play; `0` = until Ctrl+C |

Frames are streamed from your computer using the GIF's own frame timing, so the script keeps
running while the animation plays. When it stops, the last frame stays on the sign.

### `clock` — a clock the sign keeps itself

```
sign.py clock [--12h] [--tz TZ] [--template "..."]
```

Sends a **clock template** once. From then on the ESP32 keeps time itself and redraws whenever the
text changes (once a minute for `HH:MM`). Time comes from NTP: `pool.ntp.org` / `time.nist.gov`,
synced when Wi-Fi connects and again every 30 minutes. Until the first sync it shows "syncing time…".

| Option | Default | Meaning |
|---|---|---|
| `--12h` | | 12-hour time (`%l` instead of `%k`). Only applies to the default template. |
| `--tz TZ` | this computer's timezone | POSIX timezone string, e.g. `EST5EDT,M3.2.0,M11.1.0` |
| `--template "…"` | see below | Any message with date-style `%`-codes (see **Clock templates**) |

Default template: big rainbow-wave time, small date across the top:

```
{top:9}{font:bold12}{wave}%k:%M\n{top:1}{#755}{size:1}%a %b %d
```

The timezone is read from this computer (`$TZ`, or the last line of `/etc/localtime` on Linux/macOS),
so daylight saving time switches on its own. On Windows, pass `--tz`, or the sign uses its built-in
default (`TZ_INFO` in the sketch, set to US Eastern).

The clock keeps running until a new message, image or `clear` arrives. Options-only packets
(`bright N`) don't stop it.

### `clear` — blank the display

```
sign.py clear
```

### `bright` — set brightness only

```
sign.py bright N        # 0–255; the current message stays on screen
```

---

## Multi-line text

Start a new line with a real newline, or the two characters `\n` (easiest from a shell).

| Rule | Detail |
|---|---|
| Max lines | 4. Any extra line breaks become spaces. |
| Layout | The lines are stacked and centered as a block, top to bottom. Each line is centered horizontally. |
| Per-line size | `{size:N}` (1–4) sets the size of the line it's on. Lines without it use `--size`. |
| Scrolling | Each line decides for itself: with the default auto mode, only a line that's too wide scrolls. `--scroll` / `--static` apply to every line. |
| Colors | Tags carry over from one line to the next until changed. Gradients are measured per line. |
| Too tall | If the lines add up to more than the panel height, the block is aligned to the top and the bottom is cut off. |
| Blank lines | A blank line in the middle is kept as a spacer. Trailing blank lines are dropped. |
| Literal backslash | `\\` |

How much fits on a 64×32 panel:

| Layout | Height |
|---|---|
| 4 lines at size 1 | 4 × 8 = 32 px |
| 2 lines at size 2 | 2 × 16 = 32 px |
| 1 line at size 1 + 1 line at size 3 | 8 + 24 = 32 px |
| 2 lines at size 1 + 1 line at size 2 | 8 + 8 + 16 = 32 px |

```bash
./sign.py text "{size:1}{amber}EAGLES\n{green}24{gray}-{white}17"
./sign.py text --size 1 "{cyan}nas01 {green}OK\n{cyan}nas02 {red}DOWN\n{white}42°F"
printf 'line one\nline two' | ./sign.py text --size 1 -    # real newlines from stdin work too
```

---

## Fonts

Besides the classic blocky 5×7 font, the sign has smooth, proportional fonts from the Adafruit GFX
library. Narrow letters take less room, so more fits, and large sizes look like real type
instead of scaled-up blocks.

Pick a font per line with `{font:NAME}`, or for the whole message with `--font NAME` (`font=NAME` raw).

| Name | Style | Cap height (px) | `24-17` width (px) | Avg. chars across 64 px |
|---|---|---|---|---|
| `classic` | Built-in 5×7 blocks (default) | 7 × size | 30 × size | 10.7 ÷ size |
| `tiny` | Tom Thumb 3×5 pixel font | 5 | 19 | ~17 |
| `pico` | Picopixel, proportional pixel font | 5 | 19 | ~17 |
| `org` | Org_01, square pixel font | 5 | 25 | ~12 |
| `sans9` | FreeSans 9 pt | 13 | 46 | ~7 |
| `sans12` | FreeSans 12 pt | 18 | 60 | ~5.6 |
| `sans18` | FreeSans 18 pt | 26 | 88 | ~3.8 |
| `sans24` | FreeSans 24 pt | 34 | 120 | ~2.9 |
| `bold9` | FreeSans Bold 9 pt | 13 | 46 | ~7 |
| `bold12` | FreeSans Bold 12 pt | 18 | 61 | ~5 |
| `bold18` | FreeSans Bold 18 pt | 26 | 88 | ~3.5 |
| `bold24` | FreeSans Bold 24 pt | 34 | 120 | ~2.6 |
| `mono9` | FreeMono Bold 9 pt (fixed width) | 11 | 55 | ~5.8 |
| `mono12` | FreeMono Bold 12 pt (fixed width) | 14 | 70 | ~4.6 |
| `serif9` | FreeSerif Bold 9 pt | 12 | 42 | ~7.5 |
| `serif12` | FreeSerif Bold 12 pt | 16 | 56 | ~5.7 |

How fonts and sizes interact:

- With a real font, `size` is a **scale factor on top of the font** and defaults to 1. `{size:2}` doubles it.
- With `classic`, size defaults to 2 as before.
- Each line's height fits the characters it actually uses, so a line of digits is centered exactly.
  Lines in real fonts get 1 px of spacing between them.
- `24`/`34`-px fonts (`sans24`, `bold24`) are taller than a 64×32 panel; they're meant for 64×64.
  Anything taller than the panel is cut off.
- These fonts only have plain ASCII characters. `°` is drawn specially (a small ring sized to the font);
  any other missing character shows as `?`.
- An unknown font name, e.g. `{font:nope}`, is shown as plain text.

Layouts that work well on 64×32:

| Use | Message |
|---|---|
| Big score | `{font:bold12}{green}24{gray}-{white}17` |
| Label + big score | `{font:tiny}{amber}PHI v DAL\n{font:bold12}{green}24{gray}-{white}17` |
| Temperature + conditions | `{font:sans9}{cyan}42°F\n{font:pico}Light rain, 8 mph` |
| Headline ticker | `{top:1}{font:tiny}{orange}ARS 3/20\n{top:12}{font:sans9}…headline…` (or `headlines.py --font sans9 --label-font tiny`) |
| 3–4 status lines | `@font=tiny` or `@font=org`, one item per line |
| Clock | `{font:bold12}12:45` or `{font:mono9}` for fixed-width digits that don't shift |

```bash
./sign.py text --font bold12 --static "{green}24{gray}-{white}17"
./sign.py text "{font:tiny}{amber}PHI v DAL\n{font:bold12}{green}24{gray}-{white}17"
./sign.py text --font tiny "{green}nas01 OK\n{green}nas02 OK\n{red}nas03 DOWN\n{amber}cert 12d"
```

---

## Boxes

`{box:COLOR:x1,y1:x2,y2}` fills a rectangle from corner `x1,y1` to corner `x2,y2` (both included).

- `COLOR` is any color name or `#RRGGBB` / `#RGB`.
- Corners can be given in any order and are clipped to the panel, so `0,0:64,32` covers a 64×32 panel.
- Commas or colons both work between numbers (`0,0:32,32` = `0:0:32:32`).
- Boxes are always drawn first, as the background. Text goes on top, wherever the tag appears in the message.
- Up to 8 boxes per message. A message can be only boxes, with no text.
- A box tag with a bad color or missing numbers is shown as plain text.

```bash
./sign.py text --static "{box:white:0,0:64,32}{black}INVERT"            # black text on white
./sign.py text "{box:green:0,0:32,32}{box:blue:33,0:64,32}"             # left half green, right half blue
./sign.py text "{box:#203060:0,0:63,7}{top:1}{font:tiny}{white}ARS 3/20\n{top:12}{font:sans9}{amber}Headline"   # header bar
```

---

## Clock templates

Add `clock` to the options line (or use `sign.py clock --template`) and the message becomes a template:
`%`-codes are replaced with the current time on the sign. All the usual tags, fonts, boxes and
placement work. The sign only redraws when the result changes.

| Code | Example | Meaning |
|---|---|---|
| `%H` / `%k` / `%-H` | `09` / ` 9` / `9` | Hour, 24-hour: zero-padded / space-padded / no padding |
| `%I` / `%l` / `%-I` | `09` / ` 9` / `9` | Hour, 12-hour |
| `%M` | `05` | Minute |
| `%S` | `07` | Second (the sign then redraws every second) |
| `%p` | `AM` | AM / PM |
| `%a` / `%A` | `Tue` / `Tuesday` | Weekday |
| `%b` / `%B` | `Oct` / `October` | Month name |
| `%d` / `%-d` / `%e` | `03` / `3` / ` 3` | Day of month |
| `%m` | `10` | Month number |
| `%y` / `%Y` | `26` / `2026` | Year |
| `%j` | `286` | Day of the year |
| `%Z` | `EDT` | Timezone abbreviation |
| `%%` | `%` | A literal percent sign |

`-` after `%` drops leading zeros or spaces from any code (`%-M`, `%-m`, …). Other `strftime` codes
work too.

```bash
./sign.py clock
./sign.py clock --12h
./sign.py clock --template "{top:4}{font:bold12}{cyan}%-I:%M\n{top:24}{font:tiny}{amber}%A"
./sign.py clock --template "{box:#101830:0,0:63,31}{font:mono9}{white}%H:%M:%S"
printf '@clock static\n{font:bold12}%%k:%%M' | nc -u -w1 sign.local 4210     # raw (note %% for printf)
```

---

## Placement

By default, lines are stacked and centered automatically. Each line's height fits its own letters,
so with real fonts the layout can shift by a pixel or two from one message to the next
(a headline with descenders like `g`, `y` and `p` is taller than one without). For a layout that never
moves, pin lines in place:

| Tag | Static line | Scrolling line |
|---|---|---|
| `{top:N}` | Top of the capital letters goes on row N | Same |
| `{left:N}` | Left edge of the text goes at column N (instead of centered) | The scroll **window** starts at column N: text enters at the right edge and disappears at column N |

- Rows and columns start at 0 in the top-left corner. Negative values and values past the edge are allowed (the text is clipped).
- A line with `{top:}` is measured using the whole font, not just its letters, so its position never changes with content.
  Descenders hang below the capitals, and a few tall symbols reach a pixel or two above `top`.
- Lines **without** `{top:}` are still stacked and centered as a block among themselves.
- Positioned lines can overlap other lines; whatever is drawn later (lower in the message) wins.
- Tags apply to the line they're on and can go anywhere in it. Spaces after the colon are fine (`{left: 16}`).
- With a `{left:}` scroll window, a line scrolls once it's wider than the window (`64 − N`), not the whole panel.

Where capital letters sit for each font: they start on row `top` and are as tall as the font's cap height
(see the Fonts table). For example, `{top:12}{font:sans9}` puts capitals on rows 12–24, and descenders reach row ~27.

```bash
# Label pinned on row 1, headline pinned on row 12: nothing bounces between headlines
./sign.py text "{top:1}{font:tiny}{orange}ARS 3/20\n{top:12}{font:sans9}{white}Headline text here"

# Static label on the left, text scrolling only to the right of it
./sign.py text "{top:12}{left:1}{font:tiny}{orange}ARS\n{top:10}{left:20}{font:sans9}Scrolls in the right part"

# Corner labels over a big centered value
./sign.py text "{top:1}{left:1}{font:tiny}{cyan}NAS\n{top:1}{left:44}{font:tiny}{green}OK\n{top:12}{font:bold12}61%"
```

`headlines.py` pins its two lines this way. Adjust with `--label-top N` and `--headline-top N` (defaults 1 and 12).

---

## Color tags

Tags go inside the message text and apply until the next tag.

| Tag | Effect |
|---|---|
| `{red}` `{green}` `{blue}` `{yellow}` `{cyan}` `{magenta}` | Named colors |
| `{orange}` `{amber}` `{purple}` `{pink}` `{white}` `{gray}` `{grey}` `{black}` | More named colors |
| `{#FF8800}` / `{#F80}` | Any hex color (6- or 3-digit) |
| `{rainbow}` | The whole span fades through the rainbow together |
| `{wave}` | A rainbow that ripples along the text |
| `{grad:A:B}` | Fixed left-to-right gradient from color A to color B, e.g. `{grad:#FF0000:#0000FF}` or `{grad:red:blue}` |
| `{grad:rainbow}` | A full rainbow spread across the span, sliding over time |
| `{pulse:X}` | Color X with a breathing brightness. `{pulse}` alone uses the current color. |
| `{blink:X}` | Color X blinking on and off (½ s). `{blink}` alone uses the current color. |
| `{/}` | Back to the default color (`--color`, or white) |
| `{{` | A literal `{` |
| `{size:N}` | Size of the current line (see Multi-line text) |
| `{font:NAME}` | Font of the current line (see Fonts) |
| `{top:N}` | Fixed row for the current line (see Placement) |
| `{left:N}` | Fixed column for the current line (see Placement) |
| `{box:C:x1,y1:x2,y2}` | Filled rectangle in color C behind the text (see Boxes) |

Notes:

- Tag names aren't case-sensitive.
- An unknown tag such as `{bogus}` is shown as plain text.
- `°` (UTF-8 degree sign) displays correctly. Other non-ASCII characters show as `?`.
- Tabs become spaces. Line breaks (real or `\n`) start a new line.

---

## Replies and exit codes

The sign answers each text packet, and each fully received image, with:

| Reply | Meaning |
|---|---|
| `OK w=N` | Text accepted; `N` is the width in pixels of the widest line (useful for timing a scroll: it takes `(N + 64) / speed` seconds to pass) |
| `OK` | Settings-only packet accepted, or `clear` |
| `OK image` | Image fully received and shown |
| `ERR <reason>` | Rejected, e.g. `ERR image larger than panel`, `ERR empty message` |

`sign.py` prints the reply and exits with **0** for `OK` and **1** for `ERR` or no reply
(1 second timeout). The `gif` command doesn't wait for replies.

---

## Examples

```bash
# Small status line
./sign.py text --size 1 "{cyan}nas01 {green}OK {white}42°F"

# Big static word
./sign.py text --size 4 --static "{wave}HI"

# Alert that scrolls faster
./sign.py text --size 3 --speed 50 "{pulse:red}ALERT{/} disk full on {amber}nas01"

# Gradient and rainbow in one line
./sign.py text "{grad:#FF0000:#FFFF00}hot hot hot {grad:rainbow}party"

# Pipe command output in
uptime | ./sign.py text --size 1 -

# Images
./sign.py image photo.jpg
./sign.py image sprite.png --nearest
./sign.py --panel 64x64 image logo.png --fill
./sign.py gif nyan.gif --loops 3

# Brightness and clearing
./sign.py bright 20
./sign.py clear

# Use an IP address instead of sign.local
./sign.py --host 192.168.68.112 text "{green}Hello"
```

---

## Without sign.py (raw UDP)

Anything that can send a UDP packet can drive the sign.

### Text packet

UTF-8 text. An optional **first line** starting with `@` holds options; the rest is the message.

```
@scroll speed=40 size=1
{red}DISK 92%{/} on {cyan}nas01
```

| Option token | Same as |
|---|---|
| `scroll` / `static` / `auto` | `--scroll` / `--static` / (default) |
| `once` | `--once` |
| `clock` | message is a clock template (see Clock templates) |
| `setup` | reboot into Wi-Fi setup mode (`sign.py setup`) |
| `tz=POSIX_TZ` | timezone for the clock, e.g. `tz=EST5EDT,M3.2.0,M11.1.0` (sticky) |
| `speed=N` | `--speed N` |
| `size=N` | `--size N` |
| `font=NAME` | `--font NAME` |
| `gap=N` | `--gap N` |
| `color=X` | `--color X` |
| `bright=N` | `--bright N` |
| `clear` | `clear` command |

Everything after the options line is the message, including any further line breaks (one line per row).
Tokens can be separated by spaces or commas. A packet that is **only** an options line
(e.g. `@bright=30` or `@speed=60`) changes that setting and keeps the current message.

```bash
# bash
echo '{wave}Hello' | nc -u -w1 sign.local 4210
printf '@static size=1\n{amber}Build #42 passed' | nc -u -w1 sign.local 4210
```

```powershell
# PowerShell
$u = New-Object Net.Sockets.UdpClient
$b = [Text.Encoding]::UTF8.GetBytes("@size=1`n{green}Backup done")
$u.Send($b, $b.Length, 'sign.local', 4210) | Out-Null
```

### Image packet

Binary, split into chunks of at most ~1,400 data bytes each so they fit in normal packets.

| Bytes | Field | Notes |
|---|---|---|
| 0–3 | magic | ASCII `IMG1` |
| 4 | format | `0` = RGB888 (only format supported) |
| 5 | frame id | Use a new value for each image; chunks with the same id belong together |
| 6–7 | width | uint16, little-endian, ≤ panel width |
| 8–9 | height | uint16, little-endian, ≤ panel height |
| 10–13 | offset | uint32, little-endian: byte offset of this chunk's data in the image |
| 14… | data | RGB888 pixels, row-major, top-left first |

- Total image size is `width × height × 3` bytes; 64×32 is 6,144 bytes, or 5 chunks.
- The image is shown, centered, once all of its bytes have arrived. Chunks can arrive in any order.
- If a chunk is lost, that frame never completes, and the next frame id starts fresh.
- Send chunks ~2 ms apart so the ESP32's small UDP receive queue doesn't overflow.
- The sign applies gamma correction (2.2) to images. Send normal sRGB colors.

Python struct format for the header: `struct.pack("<4sBBHHI", b"IMG1", 0, frame_id, w, h, offset)`.
