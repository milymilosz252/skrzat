#pragma once
#include <Arduino.h>
#include <vector>

// One limit window rendered as a labelled progress bar.
struct UsageBar {
  String label;   // "5h"
  String right;   // "3h 35m"
  int    pct;     // 0..100 spent; <0 = unknown
};

struct ListItem {
  String   label;
  String   sub;       // right-hand state text
  uint16_t accent;    // status dot colour, 0 = no dot
  bool     chevron;   // draws ">" to hint at a submenu
};

namespace ui {

// RGB565 palette
constexpr uint16_t C_BG     = 0x0861;
constexpr uint16_t C_PANEL  = 0x2124;
constexpr uint16_t C_TEXT   = 0xFFFF;
constexpr uint16_t C_DIM    = 0x8C71;
constexpr uint16_t C_SEL    = 0x0C7F;
constexpr uint16_t C_ON     = 0x2FEB;
constexpr uint16_t C_OFF    = 0x4A69;
constexpr uint16_t C_WARN   = 0xFD20;
constexpr uint16_t C_ERR    = 0xF986;
constexpr uint16_t C_CLAUDE = 0xFC8C;
constexpr uint16_t C_BAR    = 0x3D1F;   // progress-bar blue, as in /usage

void begin();
void tick();                       // idle dimming
void wake();                       // reset the idle timer, backlight on
bool asleep();
uint8_t stageNow();
uint32_t idleMs();
uint32_t sleepAfterMsNow();
void setBrightness(uint8_t v);

// Screens
void drawList(const char* title, const std::vector<ListItem>& items,
              int cursor, int top, int rows, const char* hint);
void drawAsk(const String& title, const String& question,
             const std::vector<String>& options, int cursor);
void drawBanner(const String& text, uint16_t colour, const char* tag);
void drawMessage(const char* title, const String& body, uint16_t colour);
void drawPortal(const String& apName, const String& ip);
void drawProgress(const char* label, int pct);
void drawBuddy(const UsageBar* bars, int nBars,
               const String* notes, int nNotes, uint16_t accent,
               bool blink, bool happy, int lookX, const char* hint);

void toast(const String& msg, uint16_t colour);
bool toasting();

int  visibleRows();
void ensureVisible(int cursor, int& top, int rows);

// Home Assistant names arrive as UTF-8 Polish; fold to the glyphs our font has.
String txt(const String& in);

}  // namespace ui
