#include "ui.h"
#include "settings.h"
#include "ha_client.h"
#include "claude_link.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>

namespace ui {

static M5Canvas cv(&M5.Display);
static uint32_t lastWakeMs = 0;
static uint8_t  stage      = 0;   // 0 = active, 1 = dimmed, 2 = panel asleep
static uint8_t  effBrightness();
static String   toastMsg;
static uint16_t toastCol   = C_TEXT;
static uint32_t toastUntil = 0;

static const int W = 240, H = 135;          // landscape
static const int HEADER_H = 15, FOOTER_H = 13;
static const int ROW_H = 20;
static const int BODY_Y = HEADER_H + 1;
static const int BODY_H = H - HEADER_H - FOOTER_H - 2;

#define FONT_S &fonts::Font0   //  8px, used for the header/footer
#define FONT_M &fonts::Font2   // 16px, list rows
#define FONT_L &fonts::Font4   // 26px, big values

// ---------------------------------------------------------------- text ----

// Font0/Font2 are Latin-1 only, so Polish diacritics are folded to ASCII
// rather than rendered as tofu blocks.
String txt(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    uint8_t c = in[i];
    if (c < 0x80) { out += (char)c; continue; }
    uint8_t d = (i + 1 < in.length()) ? (uint8_t)in[i + 1] : 0;
    i++;
    if (c == 0xC4) {
      switch (d) {
        case 0x84: out += 'A'; break; case 0x85: out += 'a'; break;
        case 0x86: out += 'C'; break; case 0x87: out += 'c'; break;
        case 0x98: out += 'E'; break; case 0x99: out += 'e'; break;
        default:   out += '?';
      }
    } else if (c == 0xC5) {
      switch (d) {
        case 0x81: out += 'L'; break; case 0x82: out += 'l'; break;
        case 0x83: out += 'N'; break; case 0x84: out += 'n'; break;
        case 0x9A: out += 'S'; break; case 0x9B: out += 's'; break;
        case 0xB9: out += 'Z'; break; case 0xBA: out += 'z'; break;
        case 0xBB: out += 'Z'; break; case 0xBC: out += 'z'; break;
        default:   out += '?';
      }
    } else if (c == 0xC3) {
      switch (d) {
        case 0x93: out += 'O'; break; case 0xB3: out += 'o'; break;
        default:   out += '?';
      }
    } else {
      out += '?';
    }
  }
  return out;
}

static String ellipsize(const String& s, int maxPx) {
  if (cv.textWidth(s) <= maxPx) return s;
  String t = s;
  while (t.length() > 1 && cv.textWidth(t + "..") > maxPx) {
    t.remove(t.length() - 1);
    // Back off continuation bytes so a multi-byte character is never halved.
    while (t.length() > 1 && ((uint8_t)t[t.length() - 1] & 0xC0) == 0x80)
      t.remove(t.length() - 1);
    if (t.length() > 1 && ((uint8_t)t[t.length() - 1] & 0xC0) == 0xC0)
      t.remove(t.length() - 1);
  }
  return t + "..";
}

static int wrap(const String& s, int maxPx, String* lines, int maxLines) {
  int n = 0;
  String cur;
  int i = 0;
  while (i < (int)s.length() && n < maxLines) {
    int sp = s.indexOf(' ', i);
    String word = (sp < 0) ? s.substring(i) : s.substring(i, sp);
    String test = cur.length() ? cur + " " + word : word;
    if (cv.textWidth(test) <= maxPx) {
      cur = test;
    } else {
      if (cur.length()) { lines[n++] = cur; cur = word; }
      else              { lines[n++] = ellipsize(word, maxPx); cur = ""; }
    }
    if (sp < 0) break;
    i = sp + 1;
  }
  if (cur.length() && n < maxLines) lines[n++] = cur;
  return n;
}

// Local time, "" until NTP has synced.
static String clockStr() {
  struct tm t;
  if (!getLocalTime(&t, 5)) return "";
  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  return String(buf);
}

// -------------------------------------------------------------- chrome ----

static void header(const char* title) {
  cv.fillRect(0, 0, W, HEADER_H, C_PANEL);
  cv.setFont(FONT_S);

  if (title) {
    cv.setTextColor(C_SEL, C_PANEL);
    cv.setCursor(4, 4);
    cv.print(ellipsize(txt(title), W - 152));
  }

  int x = W - 4;

  // Clock, right-most
  String now = clockStr();
  if (now.length()) {
    x -= 30;
    cv.setTextColor(C_TEXT, C_PANEL);
    cv.setCursor(x, 4);
    cv.print(now);
    cv.setTextColor(C_DIM, C_PANEL);
    x -= 4;
  }

  // Battery
  int pct = M5.Power.getBatteryLevel();
  bool chg = M5.Power.isCharging();
  uint16_t bc = chg ? C_SEL : pct > 45 ? C_ON : pct > 18 ? C_WARN : C_ERR;
  x -= 2;  cv.fillRect(x, 6, 2, 4, C_DIM);
  x -= 18; cv.drawRect(x, 3, 18, 9, C_DIM);
           cv.fillRect(x + 1, 4, constrain(pct * 16 / 100, 0, 16), 7, bc);
  cv.setTextColor(C_DIM, C_PANEL);
  x -= 26; cv.setCursor(x, 4); cv.printf("%3d%%", pct);

  // Home Assistant reachability
  x -= 20;
  cv.fillCircle(x + 4, 7, 3, ha.online ? C_ON : (settings.configured() ? C_ERR : C_OFF));
  cv.setCursor(x + 10, 4);
  cv.print("HA");

  // Wi-Fi strength
  bool wifi = WiFi.status() == WL_CONNECTED;
  int rssi = wifi ? WiFi.RSSI() : -127;
  int bars = !wifi ? 0 : rssi > -60 ? 4 : rssi > -70 ? 3 : rssi > -80 ? 2 : 1;
  x -= 20;
  for (int b = 0; b < 4; b++) {
    int bh = 3 + b * 2;
    cv.fillRect(x + b * 4, 11 - bh, 3, bh, b < bars ? C_ON : C_OFF);
  }

  if (claudeLink.pending()) {
    x -= 34;
    cv.fillRoundRect(x, 2, 30, 11, 2, C_CLAUDE);
    cv.setTextColor(0x0000, C_CLAUDE);
    cv.setCursor(x + 4, 4);
    cv.print("ASK");
  }
}

static void footer(const char* hint) {
  cv.fillRect(0, H - FOOTER_H, W, FOOTER_H, C_PANEL);
  cv.setFont(FONT_S);
  cv.setTextColor(C_DIM, C_PANEL);
  cv.setCursor(3, H - FOOTER_H + 3);
  cv.print(hint ? hint : "gora/dol  M5:ok  M5(dl):wstecz");
}

static void overlayToast() {
  if (!toastUntil || millis() > toastUntil) return;
  int h = 22;
  int y = H - FOOTER_H - h - 3;
  cv.fillRoundRect(8, y, W - 16, h, 4, 0x0000);
  cv.drawRoundRect(8, y, W - 16, h, 4, toastCol);
  cv.setFont(FONT_M);
  cv.setTextColor(toastCol, 0x0000);
  cv.setTextDatum(middle_center);
  cv.drawString(ellipsize(toastMsg, W - 26), W / 2, y + h / 2);
  cv.setTextDatum(top_left);
}

static void push() {
  overlayToast();
  cv.pushSprite(0, 0);
}

// --------------------------------------------------------------- lifecycle -

void begin() {
  M5.Display.setRotation(settings.rotation == 3 ? 3 : 1);
  M5.Display.setBrightness(effBrightness());
  cv.setPsram(true);
  cv.setColorDepth(16);
  cv.createSprite(W, H);
  cv.setTextWrap(false);
  lastWakeMs = millis();
}

// Aggressive mode caps the backlight, which is by far the biggest draw.
static uint8_t effBrightness() {
  uint8_t b = settings.brightness;
  if (settings.powerSave == 2 && b > 90) b = 90;
  return b;
}

// Idle timeout, shortened in aggressive mode and again on a low battery.
static uint32_t sleepAfterMs() {
  if (settings.powerSave == 0 || settings.sleepSec == 0) return 0;
  uint32_t ms = settings.sleepSec * 1000UL;
  if (settings.powerSave == 2) ms = max<uint32_t>(10000, ms / 2);
  if (!M5.Power.isCharging() && M5.Power.getBatteryLevel() < 15)
    ms = max<uint32_t>(8000, ms / 2);
  return ms;
}

void setBrightness(uint8_t v) {
  settings.brightness = v;
  if (stage == 0) M5.Display.setBrightness(effBrightness());
}

void wake() {
  lastWakeMs = millis();
  if (stage == 2) M5.Display.wakeup();       // SLPOUT, then restore backlight
  if (stage) M5.Display.setBrightness(effBrightness());
  stage = 0;
}

bool asleep() { return stage == 2; }
uint8_t stageNow() { return stage; }
uint32_t idleMs() { return millis() - lastWakeMs; }
uint32_t sleepAfterMsNow() { return sleepAfterMs(); }

void tick() {
  // A pending question or banner must never be hidden behind a dark screen.
  if (claudeLink.pending() || claudeLink.notifying()) {
    lastWakeMs = millis();
    if (stage) wake();
    return;
  }
  uint32_t sleepMs = sleepAfterMs();
  if (!sleepMs) {
    if (stage) wake();
    return;
  }

  uint32_t idle = millis() - lastWakeMs;
  if (stage == 0 && idle > sleepMs / 2) {
    stage = 1;                                // half-way: fade down first
    M5.Display.setBrightness(max<int>(8, effBrightness() / 4));
  } else if (stage < 2 && idle > sleepMs) {
    stage = 2;
    M5.Display.sleep();                       // panel controller off, not just the light
  }
}

void toast(const String& msg, uint16_t colour) {
  toastMsg   = txt(msg);
  toastCol   = colour;
  toastUntil = millis() + 1400;
}

bool toasting() { return toastUntil && millis() < toastUntil; }

int visibleRows() { return BODY_H / ROW_H; }

void ensureVisible(int cursor, int& top, int rows) {
  if (cursor < top)            top = cursor;
  if (cursor >= top + rows)    top = cursor - rows + 1;
  if (top < 0)                 top = 0;
}

// ------------------------------------------------------------- screens ----

void drawList(const char* title, const std::vector<ListItem>& items,
              int cursor, int top, int rows, const char* hint) {
  cv.fillSprite(C_BG);
  header(title);

  int y0 = BODY_Y;

  if (items.empty()) {
    cv.setFont(FONT_M);
    cv.setTextColor(C_DIM, C_BG);
    cv.setTextDatum(middle_center);
    cv.drawString("(pusto)", W / 2, H / 2);
    cv.setTextDatum(top_left);
    footer(hint);
    push();
    return;
  }

  for (int r = 0; r < rows; r++) {
    int i = top + r;
    if (i >= (int)items.size()) break;
    const ListItem& it = items[i];
    int y = y0 + r * ROW_H;
    bool sel = (i == cursor);

    if (sel) cv.fillRoundRect(1, y, W - 7, ROW_H - 2, 3, C_SEL);

    int x = 6;
    if (it.accent) {
      cv.fillCircle(x + 3, y + ROW_H / 2 - 1, 3, it.accent);
      x += 12;
    }

    cv.setFont(FONT_M);
    uint16_t bg = sel ? C_SEL : C_BG;

    int subW = 0;
    if (it.sub.length()) {
      String sub = txt(it.sub);
      cv.setTextColor(sel ? 0x0000 : C_DIM, bg);
      subW = cv.textWidth(sub) + 6;
      cv.setCursor(W - 8 - subW + 4, y + 2);
      cv.print(sub);
    } else if (it.chevron) {
      cv.setTextColor(sel ? 0x0000 : C_DIM, bg);
      cv.setCursor(W - 16, y + 2);
      cv.print(">");
      subW = 14;
    }

    cv.setTextColor(sel ? 0x0000 : C_TEXT, bg);
    cv.setCursor(x, y + 2);
    cv.print(ellipsize(txt(it.label), W - 12 - x - subW));
  }

  // Scrollbar down the right edge
  if ((int)items.size() > rows) {
    int trackH = rows * ROW_H;
    int knobH  = max(8, trackH * rows / (int)items.size());
    int knobY  = y0 + (trackH - knobH) * top / max(1, (int)items.size() - rows);
    cv.fillRect(W - 3, y0, 2, trackH, C_PANEL);
    cv.fillRect(W - 3, knobY, 2, knobH, C_SEL);
  }

  footer(hint);
  push();
}

void drawAsk(const String& title, const String& question,
             const std::vector<String>& options, int cursor) {
  cv.fillSprite(0x1004);

  cv.fillRect(0, 0, W, HEADER_H, C_CLAUDE);
  cv.setFont(FONT_S);
  cv.setTextColor(0x0000, C_CLAUDE);
  cv.setCursor(4, 4);
  cv.print("CLAUDE PYTA");
  if (title.length()) {
    cv.setCursor(76, 4);
    cv.print(ellipsize(txt(title), W - 116));
  }
  int secs = claudeLink.deadlineMs > millis() ? (claudeLink.deadlineMs - millis()) / 1000 : 0;
  cv.setCursor(W - 34, 4);
  cv.printf("%3ds", secs);

  // Question on the left half, options on the right half.
  const int SPLIT = 104;
  cv.setFont(FONT_M);
  cv.setTextColor(C_TEXT, 0x1004);
  String lines[6];
  int n = wrap(txt(question), SPLIT - 8, lines, 6);
  for (int i = 0; i < n; i++) {
    cv.setCursor(4, BODY_Y + 2 + i * 15);
    cv.print(lines[i]);
  }
  cv.drawFastVLine(SPLIT, BODY_Y, BODY_H, C_PANEL);

  int rows = BODY_H / ROW_H;
  int top  = 0;
  ensureVisible(cursor, top, rows);

  for (int r = 0; r < rows; r++) {
    int i = top + r;
    if (i >= (int)options.size()) break;
    int ry = BODY_Y + r * ROW_H;
    bool sel = (i == cursor);
    if (sel) cv.fillRoundRect(SPLIT + 4, ry, W - SPLIT - 8, ROW_H - 2, 3, C_CLAUDE);
    cv.setFont(FONT_M);
    cv.setTextColor(sel ? 0x0000 : C_TEXT, sel ? C_CLAUDE : 0x1004);
    cv.setCursor(SPLIT + 8, ry + 2);
    cv.printf("%d ", i + 1);
    cv.print(ellipsize(txt(options[i]), W - SPLIT - 34));
  }

  if ((int)options.size() > rows) {
    cv.setFont(FONT_S);
    cv.setTextColor(C_DIM, 0x1004);
    cv.setCursor(SPLIT + 8, H - FOOTER_H - 10);
    cv.printf("%d/%d", cursor + 1, (int)options.size());
  }

  footer("gora/dol wybor   M5:zatwierdz");
  push();
}

void drawBanner(const String& text, uint16_t colour, const char* tag) {
  cv.fillSprite(C_BG);
  header(nullptr);

  cv.fillRoundRect(4, BODY_Y + 2, W - 8, BODY_H - 4, 5, C_PANEL);
  cv.drawRoundRect(4, BODY_Y + 2, W - 8, BODY_H - 4, 5, colour);

  cv.setFont(FONT_S);
  cv.setTextColor(colour, C_PANEL);
  cv.setCursor(12, BODY_Y + 8);
  cv.print(tag);

  cv.setFont(FONT_M);
  cv.setTextColor(C_TEXT, C_PANEL);
  String lines[4];
  int n = wrap(txt(text), W - 26, lines, 4);
  for (int i = 0; i < n; i++) {
    cv.setCursor(12, BODY_Y + 22 + i * 15);
    cv.print(lines[i]);
  }

  footer("dowolny przycisk = ok");
  push();
}

void drawMessage(const char* title, const String& body, uint16_t colour) {
  cv.fillSprite(C_BG);
  header(nullptr);
  cv.setFont(FONT_M);
  cv.setTextColor(colour, C_BG);
  cv.setCursor(6, BODY_Y + 4);
  cv.print(txt(title));
  cv.setTextColor(C_TEXT, C_BG);
  String lines[5];
  int n = wrap(txt(body), W - 12, lines, 5);
  for (int i = 0; i < n; i++) {
    cv.setCursor(6, BODY_Y + 24 + i * 15);
    cv.print(lines[i]);
  }
  footer(nullptr);
  push();
}

void drawPortal(const String& apName, const String& ip) {
  cv.fillSprite(C_BG);
  cv.fillRect(0, 0, W, HEADER_H, C_WARN);
  cv.setFont(FONT_S);
  cv.setTextColor(0x0000, C_WARN);
  cv.setCursor(4, 4);
  cv.print("KONFIGURACJA");

  cv.setFont(FONT_M);
  cv.setTextColor(C_TEXT, C_BG);
  cv.setCursor(6, BODY_Y + 4);   cv.print("1. Wi-Fi w telefonie:");
  cv.setTextColor(C_ON, C_BG);
  cv.setCursor(16, BODY_Y + 22); cv.print(apName);
  cv.setTextColor(C_TEXT, C_BG);
  cv.setCursor(6, BODY_Y + 42);  cv.print("2. Otworz w przegladarce:");
  cv.setTextColor(C_ON, C_BG);
  cv.setCursor(16, BODY_Y + 60); cv.print(ip);

  cv.setFont(FONT_S);
  cv.setTextColor(C_DIM, C_BG);
  cv.setCursor(6, BODY_Y + 82);
  cv.print("Podaj siec Wi-Fi, adres HA i token dostepu.");

  footer("PWR: anuluj");
  push();
}

// Claude, as a little fellow with a speech bubble. The bubble is where the
// usage numbers live; a pending question replaces this screen entirely.
void drawBuddy(const UsageBar* bars, int nBars,
               const String* notes, int nNotes, uint16_t accent,
               bool blink, bool happy, int lookX, const char* hint) {
  cv.fillSprite(C_BG);
  header("Claude Code");

  // Boxy little fellow, proportions taken off the reference: square corners,
  // two tall rectangular eyes, four stubby legs with a gap down the middle.
  const int bw = 56, bh = 38;
  const int bx = 8,  by = BODY_Y + (BODY_H - (bh + 4 + 8)) / 2;

  cv.fillRect(bx, by, bw, bh, accent);

  const int ew = 9, eyeGap = 16;
  const int eyeLeft = bx + (bw - (ew * 2 + eyeGap)) / 2;
  const int shift = constrain(lookX, -4, 4);      // both eyes glance together
  const int ex1 = eyeLeft + shift;
  const int ex2 = eyeLeft + ew + eyeGap + shift;
  const int eyeTop = by + 12;
  if (blink) {
    cv.fillRect(ex1, eyeTop + 5, ew, 2, 0x0000);
    cv.fillRect(ex2, eyeTop + 5, ew, 2, 0x0000);
  } else {
    // A shorter, squintier eye is how a tight limit shows on his face.
    const int eh = happy ? 11 : 6;
    cv.fillRect(ex1, eyeTop + (11 - eh) / 2, ew, eh, 0x0000);
    cv.fillRect(ex2, eyeTop + (11 - eh) / 2, ew, eh, 0x0000);
  }

  const int legY = by + bh + 4, lw = 6, lh = 8;
  cv.fillRect(bx + 8,  legY, lw, lh, accent);
  cv.fillRect(bx + 17, legY, lw, lh, accent);
  cv.fillRect(bx + 33, legY, lw, lh, accent);
  cv.fillRect(bx + 42, legY, lw, lh, accent);

  // speech bubble
  const int qx = bx + bw + 14, qy = BODY_Y + 2;
  const int qw = W - qx - 5, qh = BODY_H - 4;
  cv.fillRoundRect(qx, qy, qw, qh, 6, C_PANEL);
  cv.fillTriangle(qx, qy + 26, qx - 8, qy + 33, qx, qy + 40, C_PANEL);

  cv.setFont(FONT_M);
  int ly = qy + 5;
  const int pad = 7, trackW = qw - pad * 2;

  for (int i = 0; i < nBars && ly + 23 <= qy + qh; i++) {
    const UsageBar& b = bars[i];

    cv.setTextColor(C_TEXT, C_PANEL);
    cv.setCursor(qx + pad, ly);
    cv.print(txt(b.label) + (b.pct >= 0 ? "  " + String(b.pct) + "%" : "  ?"));

    if (b.right.length()) {
      String r = txt(b.right);
      cv.setTextColor(C_DIM, C_PANEL);
      cv.setCursor(qx + qw - pad - cv.textWidth(r), ly);
      cv.print(r);
    }
    ly += 15;

    cv.fillRoundRect(qx + pad, ly, trackW, 5, 2, C_OFF);
    if (b.pct > 0) {
      int fill = trackW * constrain(b.pct, 0, 100) / 100;
      if (fill < 3) fill = 3;                 // keep a sliver visible at 1%
      cv.fillRoundRect(qx + pad, ly, fill, 5, 2, C_BAR);
    }
    ly += 9;
  }

  for (int i = 0; i < nNotes && ly + 14 <= qy + qh; i++) {
    cv.setTextColor(nBars ? C_DIM : C_TEXT, C_PANEL);
    cv.setCursor(qx + pad, ly);
    cv.print(ellipsize(txt(notes[i]), trackW));
    ly += 15;
  }

  footer(hint);
  push();
}

void drawProgress(const char* label, int pct) {
  cv.fillSprite(C_BG);
  header(nullptr);

  cv.setFont(FONT_M);
  cv.setTextColor(C_TEXT, C_BG);
  cv.setCursor(8, BODY_Y + 12);
  cv.print(txt(label));

  int bw = W - 32, bx = 16, by = BODY_Y + 44;
  cv.drawRoundRect(bx, by, bw, 16, 3, C_DIM);
  cv.fillRoundRect(bx + 2, by + 2, (bw - 4) * constrain(pct, 0, 100) / 100, 12, 2, C_SEL);

  cv.setFont(FONT_S);
  cv.setTextColor(C_DIM, C_BG);
  cv.setCursor(bx, by + 24);
  cv.printf("%d%%", constrain(pct, 0, 100));

  footer("nie odlaczaj zasilania");
  push();
}

}  // namespace ui
