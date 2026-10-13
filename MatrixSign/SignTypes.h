// SignTypes.h - shared types. These live in a header so the Arduino IDE's
// auto-generated function prototypes can see them.
#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>

struct RGB { uint8_t r, g, b; };
struct NamedColor { const char *name; RGB c; };

enum Mode : uint8_t { M_SOLID, M_RAINBOW, M_WAVE, M_GRAD, M_GRAD_RAINBOW, M_PULSE, M_BLINK };
struct Segment { Mode mode; RGB a, b; int startCol, endCol; };
enum ScrollMode { SM_AUTO, SM_SCROLL, SM_STATIC };
enum Content { C_BLANK, C_TEXT, C_IMAGE };

struct MsgOptions {
  ScrollMode scroll = SM_AUTO;
  float speed = 30;
  bool speedSet = false;
  int size = 2;
  bool sizeSet = false;
  const GFXfont *font = nullptr;   // nullptr = classic built-in 5x7 font
  int gap = 24;
  RGB color = {255, 255, 255};
  int bright = -1;
  bool clear = false;
  bool once = false;               // scroll each line a single time, then blank it
  bool clock = false;              // message is a time template, re-rendered on the sign
  char tz[64] = "";                // POSIX TZ string, e.g. EST5EDT,M3.2.0,M11.1.0
};

struct Box { int x1, y1, x2, y2; RGB c; };   // filled rectangle, corners inclusive

struct TextLine {
  int first = 0, len = 0;          // character range in msgText
  const GFXfont *font = nullptr;   // nullptr = classic built-in font
  int size = 2;                    // scale factor
  bool sizeSet = false;            // {size:N} given on this line
  int w = 0, h = 0;                // rendered size in pixels
  int gapBelow = 0;                // extra pixels below this line
  bool topSet = false;             // {top:N}: fixed position instead of auto layout
  int top = 0;                     // panel row for the top of capital letters
  bool leftSet = false;            // {left:N}
  int left = 0;                    // static: x of the line; scrolling: left edge of scroll window
  int capOffset = 0;               // canvas row where capital letters start
  GFXcanvas1 *canvas = nullptr;    // 1-bit rendering of this line
  uint8_t *colSeg = nullptr;       // segment index for each pixel column
  bool scrolling = false;
  float scrollPos = 0;             // text column at screen x = 0
};
