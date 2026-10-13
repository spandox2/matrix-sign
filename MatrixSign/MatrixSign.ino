// MatrixSign.ino - Wi-Fi UDP message sign
// ESP32-S3-DevKitC-1 + Seengreat RGB Matrix Adapter Board (E) + HUB75 panel
//
// Listens on UDP port 4210. The last packet received is what's shown.
//
// TEXT packet (plain UTF-8):
//   optional first line of options starting with '@', then the message:
//     @scroll speed=40
//     {red}DISK 92%{/} on {cyan}nas01
//
//   Options: scroll | static | auto (default: scroll only if too wide)
//            once     (scrolling lines pass through one time, then go blank)
//            speed=N  (px/sec, default 30)     size=N (1..4, default 2)
//            gap=N    (px between repeats)     color=X (default color)
//            bright=N (0-255, sticky)          clear (blank the display)
//   A packet that is ONLY an options line (e.g. "@bright=40") adjusts
//   brightness/speed without replacing the message.
//
//   Color tags:  {red} {#FF8800} {rainbow} {wave} {grad:#F00:#00F}
//                {grad:rainbow} {pulse:red} {blink:yellow} {/}   {{ = literal {
//
//   Multi-line: a newline or the two characters \n start a new line (max 4).
//   {size:N} sets the size of the current line. Each line is centered and
//   scrolls on its own if it's too wide. \\ = literal backslash.
//
//   Fonts: {font:NAME} per line, or font=NAME in the options line.
//     classic (default, 5x7 blocks)  tiny  pico  org
//     sans9 sans12 sans18 sans24   bold9 bold12 bold18 bold24
//     mono9 mono12   serif9 serif12
//   With a real font, size defaults to 1 (it's a scale factor on top of the font).
//
//   Boxes: {box:COLOR:x1,y1:x2,y2} fills a rectangle (corners inclusive) behind the text.
//
//   Clock: "@clock" makes the message a template with date-style %-codes
//   (%H %M %S %a %b %d %k %l %p %-H ...), re-rendered on the sign as time passes.
//   Time comes from NTP (every 30 min). tz=POSIX_TZ sets the timezone (sticky).
//
//   Placement: {top:N} puts the top of the line's capital letters on row N and
//   keeps it there whatever the content. {left:N} sets the x of a static line, or
//   the left edge of a scrolling line's window. Lines without {top:} are centered.
//
// IMAGE packet (binary, may be split into chunks):
//   bytes 0-3  "IMG1"
//   byte  4    format (0 = RGB888)
//   byte  5    frame id (change it for each new image)
//   bytes 6-7  width  (uint16 LE)  <= panel width
//   bytes 8-9  height (uint16 LE)  <= panel height
//   bytes 10-13 byte offset of this chunk's data (uint32 LE)
//   bytes 14.. RGB888 pixel data, row-major
//   The image is shown (centered) once all width*height*3 bytes arrive.
//
// Wi-Fi: no secrets.h. On first boot (or when the saved network can't be found) the
// sign runs its own network "MatrixSign-XXXX"; the password and address are shown
// on the panel. Join it with a phone and pick your Wi-Fi on the page that opens.
// "@setup" reboots into setup mode; holding BOOT for 5 s forgets the network.
//
// Replies "OK ..." or "ERR ..." to the sender for text and completed images.
// For text, the reply is "OK w=N": the width in pixels of the widest line.
//
// Libraries: "ESP32 HUB75 LED MATRIX PANEL DMA Display" (mrcodetastic),
//            "Adafruit GFX Library" (Adafruit)

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <Adafruit_GFX.h>
#include <Fonts/TomThumb.h>
#include <Fonts/Picopixel.h>
#include <Fonts/Org_01.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeSerifBold9pt7b.h>
#include <Fonts/FreeSerifBold12pt7b.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "SignTypes.h"
#include "driver/gpio.h"
#include <time.h>
#include "esp_system.h"
#include "esp_sntp.h"

// ---------- Settings ----------
#define PANEL_RES_X 64      // panel width in pixels
#define PANEL_RES_Y 32      // panel height: 32 for 64x32, 64 for 64x64
#define PANEL_CHAIN 1
#define ADAPTER_V2          // comment out if your adapter board says V1.x

#define HOSTNAME "sign"     // default name (sign.local); can be changed on the setup page
#define SETUP_AP_PREFIX "MatrixSign-"   // setup-mode network name prefix
#define SETUP_RETRY_SECONDS 60          // in setup mode, retry the saved network this often
#define SETUP_FORCED_MINUTES 10         // "sign.py setup" gives up and returns after this long
#define RESET_HOLD_SECONDS 5            // hold BOOT this long to forget Wi-Fi (V2 adapter only)
#define UDP_PORT 4210
#define DEFAULT_BRIGHT 80
#define TZ_INFO  "EST5EDT,M3.2.0,M11.1.0"   // default timezone (POSIX format); sign.py clock sends yours
#define NTP_1    "pool.ntp.org"
#define NTP_2    "time.nist.gov"
#define NTP_SYNC_MINUTES 30
#define MAX_BOXES 8
#define MAX_TEXT 256        // max visible characters per message
#define MAX_SEGS 32         // max color changes per message
#define MAX_LINES 4         // max text lines per message

// ---- Wi-Fi / power tuning (see README: Troubleshooting) ----
#define WIFI_ENABLED    1    // 0 = no Wi-Fi at all; shows a local test message instead
#define WIFI_TX_POWER   WIFI_POWER_8_5dBm  // reduced transmit power = smaller supply spikes
#define WIFI_NO_SLEEP   1    // 1 = radio always on; 0 = default power-save mode
#define PIN_DRIVE       -1   // -1 = library default; 0..3 = GPIO drive strength (3 = strongest)
// ------------------------------

#ifdef ADAPTER_V2
  #define R1_PIN 18
  #define G1_PIN  8
  #define B1_PIN 17
  #define R2_PIN 16
  #define G2_PIN  1
  #define B2_PIN 15
  #define A_PIN   7
  #define B_PIN  48
  #define C_PIN   6
  #define D_PIN  47
  #define E_PIN   2
  #define LAT_PIN 21
  #define OE_PIN  4
  #define CLK_PIN 5
#else
  #define R1_PIN 37
  #define G1_PIN  6
  #define B1_PIN 36
  #define R2_PIN 35
  #define G2_PIN  5
  #define B2_PIN  0
  #define A_PIN  45
  #define B_PIN   1
  #define C_PIN  48
  #define D_PIN   2
  #define E_PIN   4
  #define LAT_PIN 38
  #define OE_PIN 21
  #define CLK_PIN 47
#endif

const int SCREEN_W = PANEL_RES_X * PANEL_CHAIN;
const int SCREEN_H = PANEL_RES_Y;

MatrixPanel_I2S_DMA *display = nullptr;

// Our own copy of the screen. Each frame is drawn here first, then only the
// pixels that changed are written to the panel (no clearing, no buffer swaps).
uint8_t frameBuf[SCREEN_H][SCREEN_W][3];   // frame being built
uint8_t shownBuf[SCREEN_H][SCREEN_W][3];   // what the panel currently shows

inline void setPx(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
  frameBuf[y][x][0] = r; frameBuf[y][x][1] = g; frameBuf[y][x][2] = b;
}
WiFiUDP udp;

// ======================= Colors =======================




const NamedColor NAMED[] = {
  {"white",   {255, 255, 255}}, {"red",    {255,   0,   0}},
  {"green",   {  0, 255,   0}}, {"blue",   {  0,   0, 255}},
  {"yellow",  {255, 255,   0}}, {"cyan",   {  0, 255, 255}},
  {"magenta", {255,   0, 255}}, {"orange", {255,  80,   0}},
  {"amber",   {255, 150,   0}}, {"purple", {140,   0, 255}},
  {"pink",    {255,  50, 120}}, {"gray",   { 80,  80,  80}},
  {"grey",    { 80,  80,  80}}, {"black",  {  0,   0,   0}},
};

RGB hueToRGB(uint32_t hue) {
  hue %= 360;
  uint8_t rem = (hue % 60) * 255 / 60;
  switch (hue / 60) {
    case 0:  return {255, rem, 0};
    case 1:  return {(uint8_t)(255 - rem), 255, 0};
    case 2:  return {0, 255, rem};
    case 3:  return {0, (uint8_t)(255 - rem), 255};
    case 4:  return {rem, 0, 255};
    default: return {255, 0, (uint8_t)(255 - rem)};
  }
}

RGB lerpRGB(RGB a, RGB b, uint8_t t) {   // t: 0 = a, 255 = b
  return { (uint8_t)(a.r + ((int)b.r - a.r) * t / 255),
           (uint8_t)(a.g + ((int)b.g - a.g) * t / 255),
           (uint8_t)(a.b + ((int)b.b - a.b) * t / 255) };
}

RGB scaleRGB(RGB c, uint8_t s) {
  return { (uint8_t)(c.r * s / 255), (uint8_t)(c.g * s / 255), (uint8_t)(c.b * s / 255) };
}

// ======================= Fonts =======================

struct NamedFont { const char *name; const GFXfont *font; };
const NamedFont FONTS[] = {
  {"classic", nullptr},             // built-in 5x7, scales in blocks with size
  {"tiny",   &TomThumb},            // 3x5 pixel font
  {"pico",   &Picopixel},           // small proportional pixel font
  {"org",    &Org_01},              // small square pixel font
  {"sans9",  &FreeSans9pt7b},   {"sans12", &FreeSans12pt7b},
  {"sans18", &FreeSans18pt7b},  {"sans24", &FreeSans24pt7b},
  {"bold9",  &FreeSansBold9pt7b},  {"bold12", &FreeSansBold12pt7b},
  {"bold18", &FreeSansBold18pt7b}, {"bold24", &FreeSansBold24pt7b},
  {"mono9",  &FreeMonoBold9pt7b},  {"mono12", &FreeMonoBold12pt7b},
  {"serif9", &FreeSerifBold9pt7b}, {"serif12", &FreeSerifBold12pt7b},
};

// Looks up a font by name. Returns false if unknown.
bool findFont(const char *name, const GFXfont *&out) {
  for (auto &f : FONTS) if (strcasecmp(name, f.name) == 0) { out = f.font; return true; }
  return false;
}

const GFXglyph *glyphFor(const GFXfont *f, uint8_t c) {
  if (c < f->first || c > f->last) c = '?';
  if (c < f->first || c > f->last) return nullptr;
  return &f->glyph[c - f->first];
}

// Height of a capital letter, used for the drawn degree sign and blank lines
int capHeight(const GFXfont *f, int size) {
  const GFXglyph *g = glyphFor(f, 'A');
  return (g && g->height) ? g->height * size : max(1, (int)f->yAdvance * size / 2);
}

// Offset from the baseline to the top of a capital letter (negative = above)
int capTopOffset(const GFXfont *f, int size) {
  const GFXglyph *g = glyphFor(f, 'A');
  return (g && g->height) ? g->yOffset * size : -capHeight(f, size);
}

// Accepts "red", "#FF8800" or "#F80".
bool parseColor(const char *s, RGB &out) {
  if (s[0] == '#') {
    size_t n = strlen(s + 1);
    for (size_t i = 1; i <= n; i++) if (!isxdigit((unsigned char)s[i])) return false;
    uint32_t v = strtoul(s + 1, nullptr, 16);
    if (n == 6) { out = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; return true; }
    if (n == 3) { out = {(uint8_t)(((v >> 8) & 0xF) * 17), (uint8_t)(((v >> 4) & 0xF) * 17),
                         (uint8_t)((v & 0xF) * 17)}; return true; }
    return false;
  }
  for (auto &nc : NAMED) if (strcasecmp(s, nc.name) == 0) { out = nc.c; return true; }
  return false;
}

// ======================= Message state =======================



Content content = C_BLANK;
uint8_t brightness = DEFAULT_BRIGHT;

// Text: all lines' characters are stored back to back in msgText
char     msgText[MAX_TEXT + 1];
int      msgLen = 0;
uint8_t  charSeg[MAX_TEXT];        // segment index for each character
Segment  segs[MAX_SEGS];
int      nSegs = 0;
TextLine lines[MAX_LINES];
int      nLines = 0;
float    speedPx = 30;
int      gapPx = 24;
bool     scrollOnce = false;       // scrolling lines pass through one time only
Box      boxes[MAX_BOXES];         // background rectangles, drawn before the text
int      nBoxes = 0;

// Clock mode: the message is a strftime-style template re-rendered whenever its output changes
bool       clockMode = false;
char       clockTpl[1024];
MsgOptions clockOpts;
char       clockLast[1024];
char       tzInfo[64] = TZ_INFO;
const char *updateClock();
bool       pendingSetupReboot = false;   // "@setup" received; reboot after replying

// Images
const size_t IMG_MAX = (size_t)SCREEN_W * SCREEN_H * 3;
uint8_t  imgRx[IMG_MAX];           // being received
uint8_t  imgShow[IMG_MAX];         // currently displayed (gamma-corrected)
int      imgW = 0, imgH = 0;
bool     rxActive = false;
uint8_t  rxFrame = 0;
int      rxW = 0, rxH = 0;
uint32_t rxBytes = 0, rxTotal = 0;
uint8_t  gammaLUT[256];

// ======================= Text parsing =======================


// Interprets the inside of a {tag}. Returns false if it isn't a known tag.
bool applyTag(const char *tag, Segment &seg, RGB defColor) {
  RGB c1, c2;
  if (strcmp(tag, "/") == 0)            { seg = {M_SOLID, defColor, defColor, 0, 0}; return true; }
  if (strcasecmp(tag, "rainbow") == 0)  { seg.mode = M_RAINBOW; return true; }
  if (strcasecmp(tag, "wave") == 0)     { seg.mode = M_WAVE; return true; }
  if (strncasecmp(tag, "grad:", 5) == 0) {
    const char *rest = tag + 5;
    if (strcasecmp(rest, "rainbow") == 0) { seg.mode = M_GRAD_RAINBOW; return true; }
    char buf[40];
    strncpy(buf, rest, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    char *colon = strchr(buf, ':');
    if (!colon) return false;
    *colon = 0;
    if (!parseColor(buf, c1) || !parseColor(colon + 1, c2)) return false;
    seg = {M_GRAD, c1, c2, 0, 0};
    return true;
  }
  if (strncasecmp(tag, "pulse", 5) == 0 || strncasecmp(tag, "blink", 5) == 0) {
    Mode m = (tolower(tag[0]) == 'p') ? M_PULSE : M_BLINK;
    if (tag[5] == 0) {                          // {pulse} keeps the current solid color
      c1 = (seg.mode == M_SOLID || seg.mode == M_PULSE || seg.mode == M_BLINK) ? seg.a : defColor;
    } else if (tag[5] != ':' || !parseColor(tag + 6, c1)) {
      return false;
    }
    seg = {m, c1, c1, 0, 0};
    return true;
  }
  if (parseColor(tag, c1)) { seg = {M_SOLID, c1, c1, 0, 0}; return true; }
  return false;
}

void parseOptions(char *line, MsgOptions &o) {
  // Tokens are separated by spaces, tabs or commas -- except tz=..., whose value contains commas
  for (char *p = line; *p; ) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (!*p) break;
    char *tok = p;
    bool isTz = strncasecmp(tok, "tz=", 3) == 0;
    while (*p && *p != ' ' && *p != '\t' && (isTz || *p != ',')) p++;
    if (*p) *p++ = 0;
    char *eq = strchr(tok, '=');
    const char *val = eq ? eq + 1 : "";
    if (eq) *eq = 0;
    if      (!strcasecmp(tok, "scroll"))  o.scroll = SM_SCROLL;
    else if (!strcasecmp(tok, "static"))  o.scroll = SM_STATIC;
    else if (!strcasecmp(tok, "auto"))    o.scroll = SM_AUTO;
    else if (!strcasecmp(tok, "clear"))   o.clear = true;
    else if (!strcasecmp(tok, "once"))    o.once = true;
    else if (!strcasecmp(tok, "clock"))   o.clock = true;
    else if (!strcasecmp(tok, "setup"))   pendingSetupReboot = true;   // reboot into Wi-Fi setup mode
    else if (!strcasecmp(tok, "tz"))      { strncpy(o.tz, val, sizeof(o.tz) - 1); o.tz[sizeof(o.tz) - 1] = 0; }
    else if (!strcasecmp(tok, "speed"))   { o.speed = constrain((float)atof(val), 1.0f, 500.0f); o.speedSet = true; }
    else if (!strcasecmp(tok, "size"))    { o.size = constrain(atoi(val), 1, 4); o.sizeSet = true; }
    else if (!strcasecmp(tok, "font"))    findFont(val, o.font);
    else if (!strcasecmp(tok, "gap"))     o.gap = constrain(atoi(val), 0, 512);
    else if (!strcasecmp(tok, "bright"))  o.bright = constrain(atoi(val), 0, 255);
    else if (!strcasecmp(tok, "color"))   parseColor(val, o.color);
  }
}

// Measure one line, render it into a 1-bit canvas, and fill in its per-column segment map.
const char *renderLine(TextLine &L, const MsgOptions &o) {
  const GFXfont *f = L.font;
  int size = f ? L.size : min(L.size, max(1, SCREEN_H / 8));   // classic: never taller than panel
  L.size = size;
  int capH = f ? capHeight(f, size) : 7 * size;
  int capY = f ? capTopOffset(f, size) : 0;                   // top of capitals, relative to baseline
  int degR = max(1, (capH + 3) / 6);                          // radius of the drawn degree sign

  // Pass 1: width and tight vertical bounds of the glyphs actually used
  int w = 0, top = INT32_MAX, bottom = INT32_MIN;
  for (int i = 0; i < L.len; i++) {
    uint8_t c = msgText[L.first + i];
    if (!f) { w += 6 * size; continue; }
    if (c == 0xF8) {                                          // degree: drawn as a small circle
      w += 2 * degR + 2 * size;
      top = min(top, capY); bottom = max(bottom, capY + 2 * degR + 1);
      continue;
    }
    const GFXglyph *g = glyphFor(f, c);
    if (!g) continue;
    w += g->xAdvance * size;
    if (g->width && g->height) {
      top = min(top, (int)g->yOffset * size);
      bottom = max(bottom, (int)(g->yOffset + g->height) * size);
    }
  }
  if (!f) { top = 0; bottom = 8 * size; }
  if (top == INT32_MAX) { top = capY; bottom = 0; }           // nothing visible (blank line)
  if (f && L.topSet) {
    // Positioned line: use the whole font's extent so the line never shifts with its content
    top = min(top, capY); bottom = max(bottom, 0);
    for (int c = f->first; c <= f->last; c++) {
      const GFXglyph *g = &f->glyph[c - f->first];
      if (!g->width || !g->height) continue;
      top = min(top, (int)g->yOffset * size);
      bottom = max(bottom, (int)(g->yOffset + g->height) * size);
    }
  }
  L.capOffset = f ? (-top + capY) : 0;                        // canvas row where capitals start
  if (f) w += size;                                           // slack for glyphs that overhang
  L.w = w;
  L.h = bottom - top;
  if (L.len == 0) return nullptr;
  int baseline = -top;

  L.canvas = new GFXcanvas1(L.w, L.h);
  L.colSeg = new uint8_t[L.w];
  if (!L.canvas || !L.canvas->getBuffer() || !L.colSeg) return "out of memory";
  GFXcanvas1 &cv = *L.canvas;
  cv.fillScreen(0);
  cv.cp437(true);
  cv.setTextWrap(false);
  cv.setFont(f);
  cv.setTextSize(size);
  cv.setTextColor(1);
  cv.setCursor(0, f ? baseline : 0);

  // Pass 2: draw each character and note which columns it covers
  int x = 0;
  for (int i = 0; i < L.len; i++) {
    uint8_t c = msgText[L.first + i];
    int x0 = x;
    if (f && c == 0xF8) {
      for (int t = 0; t < size; t++)                          // thicker ring at larger sizes
        cv.drawCircle(x0 + degR + size / 2, baseline + capY + degR, degR - t, 1);
      x = x0 + 2 * degR + 2 * size;
      cv.setCursor(x, baseline);
    } else {
      if (f && c != '?' && (c < f->first || c > f->last)) c = '?';
      cv.write(c);
      x = cv.getCursorX();
    }
    uint8_t seg = charSeg[L.first + i];
    for (int col = max(0, x0); col < min(x, L.w); col++) L.colSeg[col] = seg;
  }
  uint8_t lastSeg = charSeg[L.first + L.len - 1];
  for (int col = max(0, x); col < L.w; col++) L.colSeg[col] = lastSeg;   // slack columns

  int winW = SCREEN_W - (L.leftSet ? max(0, L.left) : 0);    // width available to this line
  L.scrolling = (o.scroll == SM_SCROLL) || (o.scroll == SM_AUTO && L.w > winW);
  L.scrollPos = L.scrolling ? -winW : 0;                     // scrolling text enters from the right
  return nullptr;
}

// Parse the tagged message body into lines / segments and render each line.
// Returns nullptr on success or an error string.
const char *buildText(const char *body, const MsgOptions &o) {
  msgLen = 0;
  nBoxes = 0;
  nSegs = 1;
  segs[0] = {M_SOLID, o.color, o.color, 0, 0};
  Segment pending = segs[0];        // style to use for the next character
  bool pendingNew = false;

  for (int l = 0; l < MAX_LINES; l++) {
    delete lines[l].canvas;
    delete[] lines[l].colSeg;
    lines[l] = TextLine();
  }
  // Defaults for a new line: message font; size from options, else 1 for real fonts / 2 for classic
  auto initLine = [&](TextLine &L, int first) {
    L.first = first;
    L.font = o.font;
    L.size = o.sizeSet ? o.size : (o.font ? 1 : o.size);
  };
  nLines = 1;
  initLine(lines[0], 0);

  auto endLine = [&]() {            // close the current line, trimming trailing spaces
    TextLine &L = lines[nLines - 1];
    while (msgLen > L.first && msgText[msgLen - 1] == ' ') msgLen--;
    L.len = msgLen - L.first;
  };

  for (const char *p = body; *p && msgLen < MAX_TEXT; ) {
    unsigned char ch = *p;
    bool newline = false;
    if (ch == '{' && p[1] == '{') {          // "{{" = literal brace
      p += 2;
    } else if (ch == '{') {
      const char *close = strchr(p, '}');
      if (close && close - p - 1 < 40) {
        char tag[40];
        memcpy(tag, p + 1, close - p - 1);
        tag[close - p - 1] = 0;
        if (strncasecmp(tag, "box:", 4) == 0) {           // {box:color:x1,y1:x2,y2}
          char spec[40];
          strncpy(spec, tag + 4, sizeof(spec) - 1); spec[sizeof(spec) - 1] = 0;
          char *sep = strchr(spec, ':');
          RGB bc;
          int v[4], nv = 0;
          if (sep) {
            *sep = 0;
            for (char *q = sep + 1; *q && nv < 4; ) {           // numbers separated by , or :
              while (*q == ',' || *q == ':' || *q == ' ') q++;
              if (!*q) break;
              char *end;
              v[nv] = strtol(q, &end, 10);
              if (end == q) break;
              nv++; q = end;
            }
          }
          if (sep && nv == 4 && parseColor(spec, bc)) {
            if (nBoxes < MAX_BOXES) boxes[nBoxes++] = {min(v[0], v[2]), min(v[1], v[3]),
                                                       max(v[0], v[2]), max(v[1], v[3]), bc};
            p = close + 1;
            continue;
          }
        }
        if (strncasecmp(tag, "top:", 4) == 0) {           // per-line fixed vertical position
          TextLine &L = lines[nLines - 1];
          L.top = constrain(atoi(tag + 4), -SCREEN_H, SCREEN_H);
          L.topSet = true;
          p = close + 1;
          continue;
        }
        if (strncasecmp(tag, "left:", 5) == 0) {          // per-line horizontal position
          TextLine &L = lines[nLines - 1];
          L.left = constrain(atoi(tag + 5), -SCREEN_W, SCREEN_W);
          L.leftSet = true;
          p = close + 1;
          continue;
        }
        if (strncasecmp(tag, "size:", 5) == 0) {          // per-line size
          lines[nLines - 1].size = constrain(atoi(tag + 5), 1, 4);
          lines[nLines - 1].sizeSet = true;
          p = close + 1;
          continue;
        }
        const GFXfont *f;
        if (strncasecmp(tag, "font:", 5) == 0 && findFont(tag + 5, f)) {   // per-line font
          TextLine &L = lines[nLines - 1];
          L.font = f;
          if (!L.sizeSet) L.size = f ? 1 : (o.sizeSet ? o.size : 2);
          p = close + 1;
          continue;
        }
        Segment s = pending;
        if (applyTag(tag, s, o.color)) {
          pending = s;
          pendingNew = true;
          p = close + 1;
          continue;
        }
      }
      p++;                                   // unknown tag: show the brace literally
    } else if (ch == '\\' && p[1] == 'n') {  // two-character "\n" = line break
      newline = true; p += 2;
    } else if (ch == '\\' && p[1] == '\\') { // "\\" = literal backslash
      p += 2;
    } else if (ch == '\n') {
      newline = true; p++;
    } else if (ch == 0xC2 && (unsigned char)p[1] == 0xB0) {
      ch = 0xF8; p += 2;                     // UTF-8 degree sign -> CP437 degree
    } else if (ch == '\r') {
      p++; continue;
    } else if (ch >= 0x80) {                 // other UTF-8: skip continuation bytes, show '?'
      p++;
      while (((unsigned char)*p & 0xC0) == 0x80) p++;
      ch = '?';
    } else {
      if (ch == '\t') ch = ' ';
      p++;
    }

    if (newline) {
      endLine();
      if (nLines < MAX_LINES) {
        initLine(lines[nLines], msgLen);
        nLines++;
        pendingNew = true;                   // each line gets its own segments (for gradients)
        continue;
      }
      ch = ' ';                              // too many lines: treat as a space
    }

    if (pendingNew) {
      // Start a new segment, or reuse the last one if it has no characters yet.
      bool lastEmpty = (msgLen == 0) || (charSeg[msgLen - 1] != nSegs - 1);
      if (lastEmpty) segs[nSegs - 1] = pending;
      else if (nSegs < MAX_SEGS) segs[nSegs++] = pending;
      pendingNew = false;
    }
    charSeg[msgLen] = nSegs - 1;
    msgText[msgLen++] = (char)ch;
  }
  endLine();
  while (nLines > 1 && lines[nLines - 1].len == 0) nLines--;   // drop trailing blank lines
  msgText[msgLen] = 0;
  if (msgLen == 0 && nBoxes == 0) return "empty message";

  // Lay out and render each line, recording which segment owns each pixel column
  for (int l = 0; l < nLines; l++) {
    const char *err = renderLine(lines[l], o);
    if (err) { content = C_BLANK; return err; }
    if (l < nLines - 1 && lines[l].font) lines[l].gapBelow = 1;   // real fonts have no built-in leading
  }

  // Column range of each segment (for gradients), measured within its line
  for (int sg = 0; sg < nSegs; sg++) { segs[sg].startCol = INT32_MAX; segs[sg].endCol = 0; }
  for (int l = 0; l < nLines; l++) {
    TextLine &L = lines[l];
    if (!L.canvas) continue;
    for (int c = 0; c < L.w; c++) {
      bool ink = false;                                       // only count columns with lit pixels
      for (int y = 0; y < L.h && !ink; y++) ink = L.canvas->getPixel(c, y);
      if (!ink) continue;
      Segment &sg = segs[L.colSeg[c]];
      sg.startCol = min(sg.startCol, c);
      sg.endCol   = max(sg.endCol, c + 1);
    }
  }

  speedPx = o.speed;
  gapPx = o.gap;
  scrollOnce = o.once;
  content = C_TEXT;
  return nullptr;
}

// Handle a text packet (null-terminated, modified in place).
const char *handleText(char *pkt) {
  MsgOptions o;
  char *body = pkt;
  if (body[0] == '@') {
    char *nl = strchr(body, '\n');
    if (nl) { *nl = 0; body = nl + 1; } else body += strlen(body);
    parseOptions(pkt + 1, o);
  }
  if (o.bright >= 0) { brightness = o.bright; display->setBrightness8(brightness); }
  if (o.tz[0] && strcmp(o.tz, tzInfo) != 0) {        // timezone change (sticky)
    strncpy(tzInfo, o.tz, sizeof(tzInfo) - 1);
    setenv("TZ", tzInfo, 1);
    tzset();
    clockLast[0] = 0;                                 // force the clock to redraw
  }
  if (o.clear) { content = C_BLANK; clockMode = false; return nullptr; }

  // Options-only packet: tweak the current message instead of replacing it
  bool hasText = false;
  for (char *p = body; *p; p++) if (!isspace((unsigned char)*p)) { hasText = true; break; }
  if (!hasText) {
    if (o.speedSet) speedPx = o.speed;
    return nullptr;
  }
  if (o.clock) {                                      // keep the template; the loop re-renders it
    strncpy(clockTpl, body, sizeof(clockTpl) - 1);
    clockTpl[sizeof(clockTpl) - 1] = 0;
    clockOpts = o;
    clockMode = true;
    clockLast[0] = 0;
    return updateClock();
  }
  clockMode = false;
  return buildText(body, o);
}

// ======================= Clock =======================

bool timeValid() { return time(nullptr) > 1700000000; }   // false until the first NTP sync

// Expand %-codes like the date command: %H %M %S %a %b %d %Y ... plus
// %k / %l (space-padded 24h / 12h hour) and a '-' flag to drop padding (%-H, %-d).
void expandTime(const char *tpl, char *out, size_t outSize, const struct tm *t) {
  size_t n = 0;
  for (const char *p = tpl; *p && n + 1 < outSize; p++) {
    if (*p != '%' || !p[1]) { out[n++] = *p; continue; }
    p++;
    bool noPad = false;
    if (*p == '-' && p[1]) { noPad = true; p++; }
    char buf[48];
    if (*p == '%') { strcpy(buf, "%"); }
    else if (*p == 'k') snprintf(buf, sizeof(buf), "%2d", t->tm_hour);
    else if (*p == 'l') snprintf(buf, sizeof(buf), "%2d", t->tm_hour % 12 ? t->tm_hour % 12 : 12);
    else {
      char fmt[3] = {'%', *p, 0};
      if (!strftime(buf, sizeof(buf), fmt, t)) buf[0] = 0;
    }
    const char *b = buf;
    if (noPad) while ((*b == '0' || *b == ' ') && b[1]) b++;   // keep at least one character
    while (*b && n + 1 < outSize) out[n++] = *b++;
  }
  out[n] = 0;
}

// Re-render the clock if its text changed (called every loop). Returns an error or nullptr.
const char *updateClock() {
  static char buf[1024];
  if (!clockMode) return nullptr;
  if (!timeValid()) {
    strcpy(buf, "{font:tiny}{amber}syncing\ntime...");
  } else {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    expandTime(clockTpl, buf, sizeof(buf), &t);
  }
  if (strcmp(buf, clockLast) == 0) return nullptr;    // nothing changed
  strcpy(clockLast, buf);
  return buildText(buf, clockOpts);
}

// ======================= Images =======================

const char *handleImageChunk(const uint8_t *buf, size_t len, bool &complete) {
  complete = false;
  if (len < 14) return "short image packet";
  if (buf[4] != 0) return "unsupported image format";
  uint8_t  frame = buf[5];
  int      w = buf[6] | (buf[7] << 8);
  int      h = buf[8] | (buf[9] << 8);
  uint32_t off = buf[10] | (buf[11] << 8) | (buf[12] << 16) | ((uint32_t)buf[13] << 24);
  const uint8_t *data = buf + 14;
  size_t   n = len - 14;

  if (w < 1 || h < 1 || w > SCREEN_W || h > SCREEN_H) return "image larger than panel";
  uint32_t total = (uint32_t)w * h * 3;

  if (!rxActive || frame != rxFrame || w != rxW || h != rxH) {   // new image starts
    rxActive = true; rxFrame = frame; rxW = w; rxH = h;
    rxTotal = total; rxBytes = 0;
  }
  if (off + n > rxTotal) return "chunk out of range";
  memcpy(imgRx + off, data, n);
  rxBytes += n;

  if (rxBytes >= rxTotal) {
    for (uint32_t i = 0; i < rxTotal; i++) imgShow[i] = gammaLUT[imgRx[i]];
    imgW = rxW; imgH = rxH;
    rxActive = false;
    content = C_IMAGE;
    clockMode = false;
    complete = true;
  }
  return nullptr;
}

// ======================= Rendering =======================

// Color for one text column. Returns false if the column should be dark (blink off).
bool columnColor(const Segment &s, int col, uint32_t now, RGB &out) {
  uint32_t hueT = now * 60 / 1000;                 // 60 degrees per second
  int span = max(1, s.endCol - s.startCol - 1);
  int pos = constrain(col - s.startCol, 0, span);
  switch (s.mode) {
    case M_SOLID:        out = s.a; return true;
    case M_RAINBOW:      out = hueToRGB(hueT); return true;
    case M_WAVE:         out = hueToRGB(hueT + (uint32_t)col * 360 / SCREEN_W); return true;
    case M_GRAD:         out = lerpRGB(s.a, s.b, pos * 255 / span); return true;
    case M_GRAD_RAINBOW: out = hueToRGB(hueT + (uint32_t)pos * 360 / (span + 1)); return true;
    case M_PULSE: {
      float ph = (now % 1500) / 1500.0f * 2 * PI;
      out = scaleRGB(s.a, (uint8_t)(50 + 205 * (0.5f + 0.5f * sinf(ph))));
      return true;
    }
    case M_BLINK:        out = s.a; return (now / 500) % 2 == 0;
  }
  return false;
}

void drawTextColumn(const TextLine &L, int sx, int col, int y0, uint32_t now) {
  RGB c;
  if (!columnColor(segs[L.colSeg[col]], col, now, c)) return;
  for (int y = 0; y < L.h; y++)
    if (L.canvas->getPixel(col, y)) setPx(sx, y0 + y, c.r, c.g, c.b);
}

void renderFrame(uint32_t now, float dtMs) {
  memset(frameBuf, 0, sizeof(frameBuf));

  if (content == C_TEXT) {
    for (int b = 0; b < nBoxes; b++) {             // background boxes first, text on top
      const Box &bx = boxes[b];
      for (int y = max(0, bx.y1); y <= min(SCREEN_H - 1, bx.y2); y++)
        for (int x = max(0, bx.x1); x <= min(SCREEN_W - 1, bx.x2); x++)
          setPx(x, y, bx.c.r, bx.c.g, bx.c.b);
    }

    // Lines without {top:} are stacked and centered as a block; {top:} lines go where they're told
    int totalH = 0;
    for (int l = 0; l < nLines; l++)
      if (!lines[l].topSet) totalH += lines[l].h + lines[l].gapBelow;
    int flowY = max(0, (SCREEN_H - totalH) / 2);   // center the block; clip if too tall

    for (int l = 0; l < nLines; l++) {
      TextLine &L = lines[l];
      int y0 = L.topSet ? L.top - L.capOffset : flowY;
      if (L.canvas) {
        if (L.scrolling) {
          int period = L.w + gapPx;
          int offset = (int)floorf(L.scrollPos);
          int winLeft = L.leftSet ? max(0, L.left) : 0;   // scroll only inside [winLeft, right edge]
          for (int sx = winLeft; sx < SCREEN_W; sx++) {
            int c = (sx - winLeft) + offset;
            if (c < 0) continue;                   // still entering from the right
            if (!scrollOnce) c %= period;          // repeat: wrap around
            if (c < L.w) drawTextColumn(L, sx, c, y0, now);
          }
          if (scrollOnce) {
            if (L.scrollPos < L.w) L.scrollPos += speedPx * dtMs / 1000.0f;   // stop once it has left
          } else {
            L.scrollPos += speedPx * dtMs / 1000.0f;
            if (L.scrollPos >= period) L.scrollPos -= period;
          }
        } else {
          int x0 = L.leftSet ? L.left : (L.w <= SCREEN_W ? (SCREEN_W - L.w) / 2 : 0);
          for (int c = 0; c < L.w && x0 + c < SCREEN_W; c++) drawTextColumn(L, x0 + c, c, y0, now);
        }
      }
      if (!L.topSet) flowY += L.h + L.gapBelow;
    }
  } else if (content == C_IMAGE) {
    int x0 = (SCREEN_W - imgW) / 2, y0 = (SCREEN_H - imgH) / 2;
    const uint8_t *p = imgShow;
    for (int y = 0; y < imgH; y++)
      for (int x = 0; x < imgW; x++, p += 3)
        setPx(x0 + x, y0 + y, p[0], p[1], p[2]);
  }

  // Push only the pixels that changed since the last frame
  for (int y = 0; y < SCREEN_H; y++)
    for (int x = 0; x < SCREEN_W; x++) {
      uint8_t *f = frameBuf[y][x], *s = shownBuf[y][x];
      if (f[0] != s[0] || f[1] != s[1] || f[2] != s[2]) {
        display->drawPixelRGB888(x, y, f[0], f[1], f[2]);
        s[0] = f[0]; s[1] = f[1]; s[2] = f[2];
      }
    }
}

// ======================= Network =======================

void reply(const char *msg) {
  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.print(msg);
  udp.endPacket();
}

void pollUdp() {
  static uint8_t pkt[1501];
  int n;
  while ((n = udp.parsePacket()) > 0) {
    int len = udp.read(pkt, sizeof(pkt) - 1);
    if (len <= 0) continue;

    if (len >= 4 && memcmp(pkt, "IMG1", 4) == 0) {
      bool complete;
      const char *err = handleImageChunk(pkt, len, complete);
      if (err) { reply((String("ERR ") + err).c_str()); rxActive = false; }
      else if (complete) reply("OK image");
    } else {
      pkt[len] = 0;
      const char *err = handleText((char *)pkt);
      if (err) {
        reply((String("ERR ") + err).c_str());
      } else if (content == C_TEXT) {           // report the widest line so scripts can time scrolling
        int maxW = 0;
        for (int l = 0; l < nLines; l++) maxW = max(maxW, lines[l].w);
        char ok[16];
        snprintf(ok, sizeof(ok), "OK w=%d", maxW);
        reply(ok);
      } else {
        reply("OK");
      }
    }
  }
}

// ======================= Wi-Fi setup mode =======================
//
// No saved network (or it can't be found): the sign runs its own access point
// "MatrixSign-XXXX" with a random password shown on the panel, plus a captive-portal
// web page for choosing a network. Settings are saved in flash (Preferences).

Preferences prefs;
WebServer   web(80);
DNSServer   dns;
String      savedSsid, savedPass;
char        hostName[25] = HOSTNAME;
bool        setupMode = false;
bool        setupRetry = false;          // retry the saved network while in setup mode
bool        setupForced = false;
uint32_t    setupStartMs = 0, lastRetryMs = 0, retryStartMs = 0;
bool        retrying = false;
bool        pendingRestart = false;
uint32_t    restartAtMs = 0;
String      apName, apPass;

void showLocal(const char *msg);
void startNetworkServices();

// Startup log line that is flushed immediately (buffered output is lost when the chip crashes)
#define LOGF(...) do { Serial.printf(__VA_ARGS__); Serial.flush(); } while (0)

void loadWifiSettings() {
  prefs.begin("sign", false);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  String h = prefs.getString("host", HOSTNAME);
  strncpy(hostName, h.length() ? h.c_str() : HOSTNAME, sizeof(hostName) - 1);
  hostName[sizeof(hostName) - 1] = 0;
}

String htmlEscape(const String &in) {
  String out;
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') out += "&amp;"; else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;"; else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;"; else out += c;
  }
  return out;
}

const char SETUP_CSS[] =
  "body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee}"
  "h1{font-size:1.3em;margin:0 0 12px}label{display:block;margin:14px 0 6px;color:#aaa}"
  "select,input{width:100%;box-sizing:border-box;padding:12px;font-size:1em;border-radius:8px;"
  "border:1px solid #444;background:#222;color:#eee}"
  "button{margin-top:20px;width:100%;padding:14px;font-size:1.05em;border:0;border-radius:8px;"
  "background:#2a7;color:#fff}a{color:#6cf}.n{color:#888;font-size:.9em;margin-top:6px}";

void handleSetupPage() {
  String page;
  page.reserve(4096);
  page += "<!doctype html><html><head><meta charset=utf-8>"
          "<meta name=viewport content='width=device-width,initial-scale=1'>"
          "<title>Matrix Sign setup</title><style>";
  page += SETUP_CSS;
  page += "</style></head><body><h1>Matrix Sign Wi-Fi setup</h1><form method=POST action=/save>";
  page += "<label>Network</label><select name=ssid>";
  int n = WiFi.scanComplete();
  for (int i = 0; i < n; i++) {
    String ss = WiFi.SSID(i);
    if (!ss.length()) continue;
    bool dup = false;                                    // list each name once (strongest first)
    for (int j = 0; j < i; j++) if (WiFi.SSID(j) == ss) { dup = true; break; }
    if (dup) continue;
    String e = htmlEscape(ss);
    page += "<option value=\"" + e + "\"" + (ss == savedSsid ? " selected" : "") + ">" + e +
            "  (" + String(WiFi.RSSI(i)) + " dBm" +
            (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? ", open" : "") + ")</option>";
  }
  page += "<option value=''>(other - type it below)</option>";   // last, so the strongest is the default
  page += "</select><div class=n><a href=/rescan>Scan again</a></div>";
  page += "<label>...or network name</label><input name=manual autocapitalize=none autocorrect=off>";
  page += "<label>Password</label><input name=pass type=password id=pw>";
  page += "<div class=n><input type=checkbox style='width:auto' "
          "onclick=\"pw.type=this.checked?'text':'password'\"> show password</div>";
  page += "<label>Sign name (hostname)</label><input name=host value=\"" + htmlEscape(hostName) +
          "\" autocapitalize=none>";
  page += "<div class=n>Reachable as <i>name</i>.local - letters, digits and dashes.</div>";
  page += "<button type=submit>Save &amp; connect</button></form></body></html>";
  web.send(200, "text/html", page);
}

void handleRescan() {
  WiFi.scanNetworks();                                   // blocking, ~2-4 s
  web.sendHeader("Location", "/");
  web.send(302, "text/plain", "");
}

void handleSave() {
  String ssid = web.arg("manual");
  ssid.trim();
  if (!ssid.length()) ssid = web.arg("ssid");
  String pass = web.arg("pass");
  String host = web.arg("host");
  host.trim();
  host.toLowerCase();
  String cleanHost;
  for (unsigned i = 0; i < host.length() && cleanHost.length() < 24; i++) {
    char c = host[i];
    if (isalnum((unsigned char)c) || c == '-') cleanHost += c;
  }
  if (!cleanHost.length()) cleanHost = HOSTNAME;

  if (!ssid.length()) {
    web.send(400, "text/html", "<meta name=viewport content='width=device-width'>"
             "<p style='font-family:sans-serif'>Pick or type a network name. <a href=/>Back</a></p>");
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("host", cleanHost);

  String page = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,"
                "initial-scale=1'><style>";
  page += SETUP_CSS;
  page += "</style><h1>Saved</h1><p>The sign is restarting and joining <b>" + htmlEscape(ssid) +
          "</b>.</p><p>Switch your phone back to your normal Wi-Fi. The sign will scroll its address "
          "(<b>" + htmlEscape(cleanHost) + ".local</b>) once it's connected.</p>"
          "<p class=n>If the password was wrong, the sign comes back to setup mode on its own.</p>";
  web.send(200, "text/html", page);

  char msg[100];
  snprintf(msg, sizeof(msg), "@static\n{font:tiny}{green}Saved!\n{font:tiny}{white}Restarting...");
  showLocal(msg);
  pendingRestart = true;                                 // let the page reach the phone first
  restartAtMs = millis() + 1500;
}

// Anything else (including phones' "is there internet?" checks) -> the setup page.
void handleCaptive() {
  web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  web.send(302, "text/plain", "");
}

void startSetupMode(bool forced) {
  setupMode = true;
  setupForced = forced;
  setupRetry = savedSsid.length() > 0 && !forced;
  setupStartMs = lastRetryMs = millis();

  LOGF("[setup] starting setup mode\n");
  WiFi.mode(WIFI_AP_STA);
  LOGF("[setup] radio on\n");
  WiFi.setAutoReconnect(false);                                // AP for the phone; STA for scans/retries
  WiFi.setTxPower(WIFI_TX_POWER);                        // keep radio current spikes down
  showLocal("@static\n{font:tiny}{amber}Wi-Fi setup\n{font:tiny}{white}scanning...");
  renderFrame(millis(), 0);
  int found = WiFi.scanNetworks();
  LOGF("[setup] scan found %d networks\n", found);

  uint64_t mac = ESP.getEfuseMac();
  char id[5];
  snprintf(id, sizeof(id), "%02X%02X", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  apName = String(SETUP_AP_PREFIX) + id;
  char pw[9];
  snprintf(pw, sizeof(pw), "%08lu", (unsigned long)(esp_random() % 100000000UL));
  apPass = pw;
  bool apOk = WiFi.softAP(apName.c_str(), apPass.c_str());
  WiFi.setTxPower(WIFI_TX_POWER);                        // re-apply: starting the AP can reset it
  LOGF("[setup] access point %s: %s\n", apOk ? "up" : "FAILED", apName.c_str());

  LOGF("[setup] starting DNS + web server\n");
  dns.start(53, "*", WiFi.softAPIP());                   // every name -> us (captive portal)
  web.on("/", HTTP_GET, handleSetupPage);
  web.on("/rescan", HTTP_GET, handleRescan);
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound(handleCaptive);
  web.begin();
  LOGF("[setup] web server running\n");

  Serial.printf("Setup mode: join %s (password %s), open http://%s/\n",
                apName.c_str(), apPass.c_str(), WiFi.softAPIP().toString().c_str());
  char msg[200];
  snprintf(msg, sizeof(msg),
           "@static\n{top:0}{left:1}{font:tiny}{amber}Wi-Fi SETUP\n"
           "{top:8}{left:1}{font:tiny}{white}%s\n"
           "{top:16}{left:1}{font:tiny}{cyan}pw %s\n"
           "{top:24}{left:1}{font:tiny}{green}%s",
           apName.c_str(), apPass.c_str(), WiFi.softAPIP().toString().c_str());
  showLocal(msg);
}

void stopSetupMode() {
  web.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  setupMode = false;
}

void setupLoop() {
  dns.processNextRequest();
  web.handleClient();
  uint32_t now = millis();

  if (pendingRestart && (int32_t)(now - restartAtMs) >= 0) ESP.restart();

  // Saved network may just be slow to come back (e.g. router rebooting after a power cut)
  if (setupRetry) {
    if (!retrying && now - lastRetryMs > SETUP_RETRY_SECONDS * 1000UL && WiFi.softAPgetStationNum() == 0) {
      WiFi.begin(savedSsid.c_str(), savedPass.c_str());
      retrying = true;
      retryStartMs = now;
    }
    if (retrying) {
      if (WiFi.status() == WL_CONNECTED) {
        stopSetupMode();
        WiFi.setHostname(hostName);
        startNetworkServices();
        return;
      }
      if (now - retryStartMs > 15000) {                  // give up this round; keep the AP stable
        WiFi.disconnect(false, false);
        retrying = false;
        lastRetryMs = now;
      }
    }
  }

  // "sign.py setup" but nobody finished it: go back to the old network
  if (setupForced && savedSsid.length() && now - setupStartMs > SETUP_FORCED_MINUTES * 60000UL &&
      WiFi.softAPgetStationNum() == 0)
    ESP.restart();
}

// Hold BOOT for a few seconds: forget the saved network and restart into setup mode.
void checkResetButton() {
#ifdef ADAPTER_V2
  static uint32_t downSince = 0;
  if (digitalRead(0) == LOW) {
    if (!downSince) downSince = millis();
    if (millis() - downSince > RESET_HOLD_SECONDS * 1000UL) {
      showLocal("@static\n{font:tiny}{red}Wi-Fi forgotten\n{font:tiny}{white}restarting...");
      renderFrame(millis(), 0);
      prefs.remove("ssid");
      prefs.remove("pass");
      delay(800);
      ESP.restart();
    }
  } else {
    downSince = 0;
  }
#endif
}

void showLocal(const char *msg) {        // display a message generated on the device
  static char buf[200];
  strncpy(buf, msg, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  handleText(buf);
}

bool netReady = false;

void startNetworkServices() {            // once Wi-Fi is up: UDP, mDNS, NTP, show our address
  static bool udpStarted = false;
  if (!udpStarted) { udp.begin(UDP_PORT); udpStarted = true; }
  sntp_set_sync_interval(NTP_SYNC_MINUTES * 60UL * 1000UL);
  configTzTime(tzInfo, NTP_1, NTP_2);
  MDNS.begin(hostName);
  MDNS.addService("sign", "udp", UDP_PORT);
  String ip = WiFi.localIP().toString();
  Serial.printf("Ready: %s.local / %s port %d\n", hostName, ip.c_str(), UDP_PORT);
  char msg[120];
  snprintf(msg, sizeof(msg), "@scroll size=1\n{green}%s.local {white}%s:%d",
           hostName, ip.c_str(), UDP_PORT);
  showLocal(msg);
  netReady = true;
}

// ======================= Setup / loop =======================

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 256; i++) gammaLUT[i] = (uint8_t)(powf(i / 255.0f, 2.2f) * 255.0f + 0.5f);

  HUB75_I2S_CFG::i2s_pins pins = {
    R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
    A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN
  };
  HUB75_I2S_CFG cfg(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN, pins);
  cfg.clkphase = false;
  cfg.double_buff = false;          // buffer swaps caused occasional ghost frames on the S3
  // Library defaults (latch_blanking > 1 can cause artifacts on some panels)
  cfg.latch_blanking = 1;
  cfg.min_refresh_rate = 60;
  display = new MatrixPanel_I2S_DMA(cfg);
  if (!display->begin()) {
    Serial.println("Matrix init failed");
    while (true) delay(1000);
  }
  display->setBrightness8(brightness);
  display->clearScreen();
#if PIN_DRIVE >= 0
  { const int8_t hubPins[] = {R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN, A_PIN, B_PIN,
                              C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN};
    for (int8_t p : hubPins) gpio_set_drive_capability((gpio_num_t)p, (gpio_drive_cap_t)PIN_DRIVE); }
#endif
  memset(shownBuf, 0, sizeof(shownBuf));
  LOGF("[boot] display ok\n");

  // Why did we (re)start? A brownout means the power supply dipped (often when Wi-Fi transmits).
  esp_reset_reason_t why = esp_reset_reason();
  const char *whyName = why == ESP_RST_POWERON ? "power-on" : why == ESP_RST_BROWNOUT ? "BROWNOUT (power dip)" :
                        why == ESP_RST_SW ? "software restart" : why == ESP_RST_PANIC ? "crash (panic)" :
                        why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT ? "watchdog" :
                        why == ESP_RST_EXT ? "reset button" : "other";
  LOGF("[boot] reset reason: %s (%d)\n", whyName, (int)why);
  if (why == ESP_RST_BROWNOUT) {
    showLocal("@static\n{font:tiny}{red}BROWNOUT RESET\n{font:tiny}{white}check power");
    for (int i = 0; i < 60; i++) { renderFrame(millis(), 16); delay(50); }   // show it ~3 s
  } else if (why == ESP_RST_PANIC) {
    showLocal("@static\n{font:tiny}{red}CRASHED\n{font:tiny}{white}see serial log");
    for (int i = 0; i < 60; i++) { renderFrame(millis(), 16); delay(50); }
  }
  LOGF("[boot] drawing first message\n");
  showLocal("@static\n{font:tiny}{white}starting...");
  renderFrame(millis(), 0);
  LOGF("[boot] first message drawn\n");

#if !WIFI_ENABLED
  showLocal("@scroll\n{wave}Wi-Fi OFF test {red}red {grad:#0000FF:#00FF00}gradient {rainbow}rainbow");
  return;
#endif
  LOGF("[boot] reading saved settings\n");
  loadWifiSettings();
  LOGF("[boot] saved network: '%s', name: %s\n", savedSsid.c_str(), hostName);
#ifdef ADAPTER_V2
  pinMode(0, INPUT_PULLUP);              // BOOT button (free on the V2 adapter)
#endif

  bool forced = prefs.getBool("force", false);
  if (forced) prefs.putBool("force", false);   // one-shot: the next boot is normal again

  bool connected = false;
  if (savedSsid.length() && !forced) {
    char msg[120];
    snprintf(msg, sizeof(msg), "@static\n{font:tiny}{blue}Joining Wi-Fi\n{font:tiny}{white}%s",
             savedSsid.c_str());
    showLocal(msg);
    renderFrame(millis(), 0);

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostName);
    WiFi.setSleep(WIFI_NO_SLEEP ? false : true);
    WiFi.setAutoReconnect(true);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    WiFi.setTxPower(WIFI_TX_POWER);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      renderFrame(millis(), 16);
      delay(16);
    }
    connected = WiFi.status() == WL_CONNECTED;
    LOGF("[boot] join %s\n", connected ? "ok" : "failed (not found / wrong password)");
  }

  // Note: no network calls (like udp.begin) before the radio is started -- the ESP32's
  // network stack doesn't exist yet and the chip asserts in xQueueSemaphoreTake.
  if (connected) startNetworkServices();
  else startSetupMode(forced);
}

void loop() {
  static uint32_t last = millis();

#if WIFI_ENABLED
  if (setupMode) {
    setupLoop();
  } else {
    // Wi-Fi came up after the boot timeout: finish setup once
    if (!netReady && WiFi.status() == WL_CONNECTED) startNetworkServices();
    pollUdp();
  }
  checkResetButton();
  if (pendingSetupReboot) {               // "@setup" received: reply has gone out, now reboot
    prefs.putBool("force", true);
    delay(300);
    ESP.restart();
  }
#endif

  static uint32_t lastClock = 0;
  if (clockMode && millis() - lastClock >= 200) {   // checks 5x/second; redraws only on change
    lastClock = millis();
    updateClock();
  }

  uint32_t now = millis();
  if (now - last >= 16) {            // ~60 fps
    renderFrame(now, now - last);
    last = now;
  } else {
    delay(1);
  }
}
