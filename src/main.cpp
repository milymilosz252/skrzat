// M5StickC Plus2 -> Home Assistant remote + Claude Code answer picker.
//
//   Landscape screen. Scrolling uses the two small buttons, the big M5
//   button on the face confirms:
//     top button    -> up      (hold to repeat)
//     side button   -> down    (hold to repeat)
//     M5 button     -> select / run;  hold -> back one level
//   "Ustawienia -> Zamien gora/dol" flips the two scroll buttons.
//
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

#include "settings.h"
#include "ha_client.h"
#include "claude_link.h"
#include "ui.h"
#include "i18n.h"
#include <ESPmDNS.h>

static const char* FW_VERSION = "1.1.0";
static const char* FW_BUILD   = __DATE__ " " __TIME__;
static const char* AP_NAME    = "Skrzat-Setup";
static const char* HOSTNAME   = "skrzat";

enum Screen { SCR_MAIN, SCR_HA, SCR_BUDDY, SCR_WIFI, SCR_CONFIRM, SCR_LIST, SCR_ENTITY, SCR_SETTINGS, SCR_INFO, SCR_ASK, SCR_NOTIFY };

struct Frame {
  Screen screen = SCR_MAIN;
  String title  = "Pilot HA";
  String filter;          // "d:light" | "a:Sypialnia" | "*" | entity id for SCR_ENTITY
  int    cursor = 0;
  int    top    = 0;
};

static std::vector<Frame>   stack;
static std::vector<ListItem> items;
static std::vector<String>   rowIds;
static Action  actions[10];
static int     actionCount = 0;
static uint32_t lastPollMs = 0;
static uint32_t pendingRefreshAt = 0;
static bool     needRedraw = true;
static Screen   preAskScreen = SCR_MAIN;

static Frame& top() { return stack.back(); }

// Wi-Fi needs at least 80 MHz; anything above that is wasted while we are
// only waiting for a button or an HTTP request.
// Keeps the link to Claude Code alive across AP hiccups. Runs even while the
// screen sleeps - a question must always be able to reach us.
// Home Assistant advertises _home-assistant._tcp. Finding it saves the user
// from typing an address; the token still has to be entered by hand.
static String discoverHa() {
  int n = MDNS.queryService("home-assistant", "tcp");
  if (n <= 0) return "";
  return "http://" + MDNS.IP(0).toString() + ":" + String(MDNS.port(0));
}

static void networkWatchdog() {
  static uint32_t lastCheck = 0;
  static uint32_t offlineSince = 0;

  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (offlineSince) {
      offlineSince = 0;
      claudeLink.reconnects++;
      claudeLink.restartMdns();
    }
    return;
  }

  if (!offlineSince) offlineSince = millis();
  WiFi.reconnect();
  // If the network never comes back, a reboot re-runs the full join sequence.
  if (millis() - offlineSince > 180000UL) ESP.restart();
}

static void setCpuFast(bool fast) {
  static int current = 0;
  int want = fast ? 240 : 80;
  if (current == want) return;
  current = want;
  setCpuFrequencyMhz(want);
}

// ------------------------------------------------------------------ utils --

static void beep(uint16_t freq = 2200, uint32_t ms = 80) {
  if (!settings.beep) return;
  M5.Speaker.setVolume(settings.volume);
  M5.Speaker.tone(freq, ms);
}

// A new question deserves more than a click: two rising tones, loud.
static void beepAsk() {
  if (!settings.beep) return;
  M5.Speaker.setVolume(settings.volume);
  M5.Speaker.tone(2200, 130);
  delay(160);
  M5.Speaker.tone(3100, 220);
}

static String stateLabel(const Entity& e) {
  if (e.isUnavailable()) return "n/d";
  if (e.domain == "light" && e.isOn() && e.brightness > 0)
    return String((e.brightness * 100 + 127) / 255) + "%";
  if (e.state == "on")   return "wl";
  if (e.state == "off")  return "wyl";
  if (e.state.length() > 6) return e.state.substring(0, 6);
  return e.state;
}

static uint16_t stateColour(const Entity& e) {
  if (e.isUnavailable()) return ui::C_ERR;
  return e.isOn() ? ui::C_ON : ui::C_OFF;
}

// ------------------------------------------------------------ list builders -

static void pushFrame(Screen s, const String& title, const String& filter) {
  Frame f;
  f.screen = s;
  f.title  = title;
  f.filter = filter;
  stack.push_back(f);
  needRedraw = true;
}

static void popFrame() {
  if (stack.size() > 1) stack.pop_back();
  needRedraw = true;
}

static void addRow(const String& id, const String& label, const String& sub,
                   uint16_t accent, bool chevron) {
  rowIds.push_back(id);
  items.push_back(ListItem{label, sub, accent, chevron});
}

static void buildMain() {
  top().title = T(S_HOME);

  addRow("@ha", T(S_HA),
         ha.online ? String(ha.entities.size()) + " " + T(S_ENTITIES) : "offline",
         ha.online ? ui::C_ON : ui::C_ERR, true);

  const char* claudeState = claudeLink.pending() ? "pyta" :
                            claudeLink.asksServed ? "gotowy" : "czeka";
  addRow("@claude", T(S_CLAUDE), claudeState,
         claudeLink.pending() ? ui::C_CLAUDE : ui::C_OFF, true);

  addRow("@settings", T(S_SETTINGS), "", ui::C_DIM, true);
}

// Everything Home Assistant, one level down.
static void buildHa() {
  auto cat = [&](const char* domain, const char* label) {
    size_t n = ha.countDomain(domain);
    if (n) addRow(String("@d:") + domain, label, String(n), 0, true);
  };
  cat("light", T(S_LIGHTS));
  cat("scene", T(S_SCENES));
  cat("automation", T(S_AUTOMATIONS));
  cat("script", T(S_SCRIPTS));
  cat("media_player", T(S_MEDIA));
  cat("switch", T(S_SWITCHES));
  addRow("@lightsoff", T(S_ALL_OFF), "", ui::C_WARN, false);
  addRow("@areas",     T(S_ROOMS), "", 0, true);
  addRow("@domains",   T(S_CATEGORIES),     "", 0, true);
  addRow("@all",       T(S_ALL_ENTITIES), String(ha.entities.size()), 0, true);
}

static void buildEntityList(const String& filter) {
  for (auto& e : ha.entities) {
    bool match = false;
    if (filter == "*")                    match = true;
    else if (filter.startsWith("d:"))     match = (e.domain == filter.substring(2));
    else if (filter.startsWith("a:"))     match = (e.area   == filter.substring(2));
    if (!match) continue;
    addRow(e.id, e.name, stateLabel(e), stateColour(e), false);
  }
}

static void buildAreas() {
  for (auto& a : ha.areas()) {
    int n = 0;
    for (auto& e : ha.entities) if (e.area == a) n++;
    addRow("@a:" + a, a, String(n), 0, true);
  }
  int orphans = 0;
  for (auto& e : ha.entities) if (!e.area.length()) orphans++;
  if (orphans) addRow("@a:", T(S_NO_ROOM), String(orphans), 0, true);
}

static void buildDomains() {
  std::vector<String> seen;
  for (auto& e : ha.entities) {
    bool dup = false;
    for (auto& s : seen) if (s == e.domain) { dup = true; break; }
    if (dup) continue;
    seen.push_back(e.domain);
    addRow("@d:" + e.domain, HaClient::domainLabel(e.domain.c_str()),
           String(ha.countDomain(e.domain.c_str())), 0, true);
  }
}

static void buildEntityActions(const String& entityId) {
  Entity* e = ha.find(entityId);
  if (!e) { addRow("", "Encja zniknela", "", ui::C_ERR, false); return; }

  top().title = e->name + "  [" + stateLabel(*e) + "]";
  actionCount = HaClient::actionsFor(*e, actions, 10);
  for (int i = 0; i < actionCount; i++)
    addRow("!" + String(i), actions[i].label, "", 0, false);
  if (!actionCount) addRow("", "(brak akcji)", "", ui::C_DIM, false);
  addRow("@id", e->id, "", ui::C_DIM, false);
}

static void buildSettings() {
  addRow("@refresh", T(S_REFRESH), "", 0, false);
  addRow("@bright",  T(S_BRIGHTNESS), String(settings.brightness), 0, false);
  addRow("@sleep",   T(S_SLEEP), String(settings.sleepSec) + "s", 0, false);
  addRow("@sound",   T(S_SOUND), settings.beep ? T(S_YES) : T(S_NO), 0, false);
  addRow("@vol",     T(S_VOLUME), String(settings.volume), 0, false);
  addRow("@power",   T(S_POWER),
         settings.powerSave == 0 ? "off" : settings.powerSave == 1 ? "norm" : "max",
         settings.powerSave == 2 ? ui::C_ON : 0, false);
  addRow("@lang",    T(S_LANGUAGE), settings.language ? "EN" : "PL", 0, false);
  addRow("@swap",    T(S_SWAP), settings.swapUpDown ? T(S_YES) : T(S_NO), 0, false);
  addRow("@rot",     T(S_ROTATE), String(settings.rotation), 0, false);
  addRow("@findha",  T(S_FIND_HA), "", 0, false);
  addRow("@wifiscan", T(S_WIFI_NETS), "", 0, true);
  addRow("@portal",  T(S_WIFI_CHANGE), "", ui::C_WARN, false);
  addRow("@info",    T(S_INFO), "", 0, true);
  addRow("@factory", T(S_FACTORY), "", ui::C_ERR, true);
  addRow("@reboot",  T(S_REBOOT), "", ui::C_WARN, false);
}

// Read-only list of what is on the air, for diagnosing a bad connection.
static void buildWifiScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); n = WIFI_SCAN_RUNNING; }
  if (n == WIFI_SCAN_RUNNING) {
    addRow("", T(S_SCANNING), "", ui::C_DIM, false);
    return;
  }
  for (int i = 0; i < n && i < 16; i++) {
    int rssi = WiFi.RSSI(i);
    uint16_t c = rssi > -60 ? ui::C_ON : rssi > -75 ? ui::C_WARN : ui::C_ERR;
    bool mine = WiFi.SSID(i) == WiFi.SSID();
    addRow("", (mine ? "* " : "") + WiFi.SSID(i), String(rssi), c, false);
  }
  if (!n) addRow("", T(S_EMPTY), "", ui::C_DIM, false);
}

static void buildConfirm() {
  addRow("@cancel",  T(S_NO), "", 0, false);
  addRow("@doreset", T(S_YES), "", ui::C_ERR, false);
}

// "za 2h14" / "3d 5h" until an absolute reset timestamp.
static String untilStr(uint32_t resetEpoch) {
  if (!resetEpoch) return "?";
  time_t now = time(nullptr);
  if (now < 1600000000) return "?";          // clock not synced yet
  long left = (long)resetEpoch - (long)now;
  if (left <= 0) return "0m";
  int days = left / 86400, hours = (left % 86400) / 3600, mins = (left % 3600) / 60;
  char buf[16];
  if (days)       snprintf(buf, sizeof(buf), "%dd %dh", days, hours);
  else if (hours) snprintf(buf, sizeof(buf), "%dh %02dm", hours, mins);
  else            snprintf(buf, sizeof(buf), "%dm", mins);
  return String(buf);
}

static String tokensStr(uint64_t n) {
  char buf[16];
  if (n >= 1000000000ULL)   snprintf(buf, sizeof(buf), "%.1fB", n / 1e9);
  else if (n >= 1000000ULL) snprintf(buf, sizeof(buf), "%.0fM", n / 1e6);
  else if (n >= 1000ULL)    snprintf(buf, sizeof(buf), "%.0fk", n / 1e3);
  else snprintf(buf, sizeof(buf), "%llu", (unsigned long long)n);
  return String(buf);
}

// What Claude "says" in the bubble: one bar per limit window, plus notes.
static int buddyContent(UsageBar* bars, int maxBars,
                        String* notes, int maxNotes, int& nNotes, bool& happy) {
  int nBars = 0;
  nNotes = 0;
  happy = true;

  if (!claudeLink.hasUsage()) {
    happy = false;
    if (nNotes < maxNotes) notes[nNotes++] = String(T(S_LIMITS)) + ": " + T(S_NO_DATA);
    if (nNotes < maxNotes) notes[nNotes++] = "skrzat usage";
  } else if (claudeLink.usageStale) {
    happy = false;
    if (nNotes < maxNotes) notes[nNotes++] = String(T(S_LIMITS)) + ": " + T(S_OUTDATED);
    if (nNotes < maxNotes) notes[nNotes++] = "skrzat usage";
  } else {
    if (nBars < maxBars && claudeLink.usageFiveHourUsed >= 0)
      bars[nBars++] = UsageBar{T(S_USED_5H), untilStr(claudeLink.usageFiveHourReset),
                                   claudeLink.usageFiveHourUsed};
    if (nBars < maxBars && claudeLink.usageSevenDayUsed >= 0)
      bars[nBars++] = UsageBar{T(S_USED_7D), untilStr(claudeLink.usageSevenDayReset),
                                   claudeLink.usageSevenDayUsed};
    happy = claudeLink.usageFiveHourUsed < 0 || claudeLink.usageFiveHourUsed <= 80;
  }

  if (nNotes < maxNotes && claudeLink.usageTokensToday)
    notes[nNotes++] = String(T(S_TODAY)) + " " + tokensStr(claudeLink.usageTokensToday);
  if (nNotes < maxNotes && !nBars)
    notes[nNotes++] = String(claudeLink.asksServed) + " " + T(S_ASKS_SESSION);
  return nBars;
}

static void buildClaude() {
  const char* st = claudeLink.pending() ? "pyta" :
                   claudeLink.status == ASK_ANSWERED ? "odp." :
                   claudeLink.status == ASK_TIMEOUT  ? "timeout" : "czeka";
  addRow("", "Status", st, claudeLink.pending() ? ui::C_CLAUDE : ui::C_OFF, false);
  addRow("", "Pytan", String(claudeLink.asksServed), 0, false);

  // --- limity uzycia -----------------------------------------------------
  if (!claudeLink.hasUsage()) {
    addRow("", "Limity", "brak danych", ui::C_DIM, false);
  } else if (claudeLink.usageStale) {
    addRow("", "Limity", "nieaktualne", ui::C_WARN, false);
    addRow("", "  odswiez", "/usage", ui::C_DIM, false);
  } else {
    auto pct = [](int used) {
      return used < 0 ? String("?") : String(used) + "%";
    };
    auto colour = [](int used) {
      return used < 0 ? ui::C_DIM : used > 85 ? ui::C_ERR
                                  : used > 65 ? ui::C_WARN : ui::C_ON;
    };
    addRow("", "5h zuzyte",   pct(claudeLink.usageFiveHourUsed),
           colour(claudeLink.usageFiveHourUsed), false);
    addRow("", "5h reset za", untilStr(claudeLink.usageFiveHourReset), 0, false);
    addRow("", "7d zuzyte",   pct(claudeLink.usageSevenDayUsed),
           colour(claudeLink.usageSevenDayUsed), false);
    addRow("", "7d reset za", untilStr(claudeLink.usageSevenDayReset), 0, false);
  }
  if (claudeLink.usageTokensToday)
    addRow("", "Tokeny dzis", tokensStr(claudeLink.usageTokensToday), 0, false);

  // --- polaczenie --------------------------------------------------------
  addRow("", "Host", ui::txt(String(HOSTNAME)), 0, false);
  addRow("", "IP", WiFi.localIP().toString(), 0, false);
  if (claudeLink.question.length())
    addRow("@lastask", "Ostatnie pytanie", "", ui::C_CLAUDE, true);
}

static void rebuild() {
  items.clear();
  rowIds.clear();
  actionCount = 0;

  switch (top().screen) {
    case SCR_MAIN:     buildMain(); break;
    case SCR_HA:       buildHa(); break;
    case SCR_LIST:
      if (top().filter == "@areas")        buildAreas();
      else if (top().filter == "@domains") buildDomains();
      else                                 buildEntityList(top().filter);
      break;
    case SCR_ENTITY:   buildEntityActions(top().filter); break;
    case SCR_SETTINGS: buildSettings(); break;
    case SCR_WIFI:     buildWifiScan(); break;
    case SCR_CONFIRM:  buildConfirm(); break;
    case SCR_INFO:     buildClaude(); break;
    default: break;
  }

  if (top().cursor >= (int)items.size()) top().cursor = max(0, (int)items.size() - 1);
  ui::ensureVisible(top().cursor, top().top, ui::visibleRows());
}

// --------------------------------------------------------------- actions ---

static void doRefresh(bool quiet = false) {
  bool ok = ha.refresh();
  if (!ok && !quiet) ui::toast(ha.lastError, ui::C_ERR);
  needRedraw = true;
}

static void activate() {
  if (rowIds.empty()) return;
  String id = rowIds[top().cursor];

  if (id.length() == 0) return;

  if (id.startsWith("!")) {                       // action on the current entity
    int idx = id.substring(1).toInt();
    Entity* e = ha.find(top().filter);
    if (!e || idx >= actionCount) return;
    beep(2600, 60);
    bool ok = ha.runAction(*e, actions[idx]);
    ui::toast(ok ? String(actions[idx].label) : ha.lastError, ok ? ui::C_ON : ui::C_ERR);
    pendingRefreshAt = millis() + 700;
    return;
  }

  if (id.startsWith("@d:")) {
    pushFrame(SCR_LIST, HaClient::domainLabel(id.substring(3).c_str()), "d:" + id.substring(3));
    return;
  }
  if (id.startsWith("@a:")) {
    String area = id.substring(3);
    pushFrame(SCR_LIST, area.length() ? area : "Bez pomieszczenia", "a:" + area);
    return;
  }
  if (id == "@areas")   { pushFrame(SCR_LIST, "Pomieszczenia", "@areas"); return; }
  if (id == "@domains") { pushFrame(SCR_LIST, "Kategorie", "@domains"); return; }
  if (id == "@all")     { pushFrame(SCR_LIST, "Wszystkie encje", "*"); return; }
  if (id == "@ha")      { pushFrame(SCR_HA, "Home Assistant", ""); return; }
  if (id == "@claude")  { pushFrame(SCR_BUDDY, "Claude Code", ""); return; }
  if (id == "@settings"){ pushFrame(SCR_SETTINGS, "Ustawienia", ""); return; }

  if (id == "@lightsoff") {
    beep(1500, 120);
    bool ok = ha.callService("light", "turn_off", "all");
    ui::toast(ok ? "Zgaszone" : ha.lastError, ok ? ui::C_ON : ui::C_ERR);
    pendingRefreshAt = millis() + 700;
    return;
  }
  if (id == "@refresh") { doRefresh(); ui::toast("Odswiezono", ui::C_ON); return; }
  if (id == "@bright") {
    uint8_t v = settings.brightness >= 250 ? 20 : settings.brightness + 45;
    ui::setBrightness(v);
    settings.save();
    needRedraw = true;
    return;
  }
  if (id == "@sleep") {
    settings.sleepSec = settings.sleepSec == 0 ? 15 :
                        settings.sleepSec >= 120 ? 0 : settings.sleepSec * 2;
    settings.save();
    needRedraw = true;
    return;
  }
  if (id == "@sound")   { settings.beep = !settings.beep; settings.save(); needRedraw = true; return; }
  if (id == "@lang") {
    langSet(settings.language ? 0 : 1);
    settings.save();
    needRedraw = true;
    return;
  }
  if (id == "@findha") {
    ui::toast(T(S_FIND_HA), ui::C_SEL);
    String url = discoverHa();
    if (url.length()) {
      settings.haUrl = url;
      settings.save();
      doRefresh(true);
      ui::toast(String(T(S_FOUND)) + ": " + url, ui::C_ON);
    } else {
      ui::toast(T(S_NOT_FOUND), ui::C_ERR);
    }
    needRedraw = true;
    return;
  }
  if (id == "@wifiscan") {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    pushFrame(SCR_WIFI, T(S_WIFI_NETS), "");
    return;
  }
  if (id == "@factory") { pushFrame(SCR_CONFIRM, T(S_FACTORY_CONFIRM), ""); return; }
  if (id == "@cancel")  { popFrame(); return; }
  if (id == "@doreset") {
    ui::drawMessage(T(S_FACTORY), T(S_RESTARTING), ui::C_ERR);
    settings.factoryReset();
    WiFi.disconnect(true, true);          // drop the stored credentials too
    delay(800);
    ESP.restart();
  }
  if (id == "@power") {
    settings.powerSave = (settings.powerSave + 1) % 3;
    settings.save();
    ui::wake();
    needRedraw = true;
    return;
  }
  if (id == "@vol") {
    settings.volume = settings.volume >= 255 ? 64 : min(255, settings.volume + 64);
    settings.save();
    beepAsk();
    needRedraw = true;
    return;
  }
  if (id == "@swap")   {
    settings.swapUpDown = !settings.swapUpDown;
    settings.save();
    ui::toast(settings.swapUpDown ? "Zamienione" : "Domyslne", ui::C_ON);
    needRedraw = true;
    return;
  }
  if (id == "@rot") {
    settings.rotation = (settings.rotation == 1) ? 3 : 1;
    settings.save();
    M5.Display.setRotation(settings.rotation);
    needRedraw = true;
    return;
  }
  if (id == "@info")   { pushFrame(SCR_INFO, "Claude Code", ""); return; }
  if (id == "@reboot") { ui::toast("Restart...", ui::C_WARN); delay(600); ESP.restart(); }
  if (id == "@portal") {
    settings.clearHa();
    ui::toast("Restart do konfiguracji", ui::C_WARN);
    delay(800);
    ESP.restart();
  }
  if (id == "@lastask") {
    ui::drawBanner(claudeLink.question.length() ? claudeLink.question : "(brak)",
                   ui::C_CLAUDE, "OSTATNIE PYTANIE");
    delay(2500);
    needRedraw = true;
    return;
  }

  // Plain entity id -> open its action list
  if (ha.find(id)) {
    pushFrame(SCR_ENTITY, id, id);
    ha.refreshOne(id);
    return;
  }
}

// Pressing B on a light/switch row toggles straight away; everything else
// opens the detail view.
static void quickOrOpen() {
  if (rowIds.empty()) return;
  String id = rowIds[top().cursor];
  if (top().screen == SCR_LIST && id.length() && id[0] != '@') {
    Entity* e = ha.find(id);
    if (e && (e->domain == "light" || e->domain == "switch" ||
              e->domain == "input_boolean" || e->domain == "fan")) {
      beep(2600, 60);
      bool ok = ha.callService(e->domain, "toggle", e->id);
      ui::toast(ok ? (e->isOn() ? "Wylaczono" : "Wlaczono") : ha.lastError,
                ok ? ui::C_ON : ui::C_ERR);
      pendingRefreshAt = millis() + 700;
      return;
    }
  }
  activate();
}

// ------------------------------------------------------------------ input --

static void moveCursor(int delta) {
  ui::wake();                      // scrolling counts as activity
  if (items.empty()) return;
  int n = (int)items.size();
  top().cursor = (top().cursor + delta % n + n) % n;
  ui::ensureVisible(top().cursor, top().top, ui::visibleRows());
  needRedraw = true;
}

// The two scroll buttons, in the order the user asked for them.
static m5::Button_Class& upButton()   { return settings.swapUpDown ? M5.BtnPWR : M5.BtnB; }
static m5::Button_Class& downButton() { return settings.swapUpDown ? M5.BtnB : M5.BtnPWR; }

// Repeats while a scroll button is held down.
static void scrollRepeat(m5::Button_Class& b, int delta) {
  static uint32_t lastRepeat = 0;
  if (b.isPressed() && b.pressedFor(500) && millis() - lastRepeat > 130) {
    lastRepeat = millis();
    moveCursor(delta);
  }
}

static void moveAsk(int delta) {
  int n = max<int>(1, claudeLink.options.size());
  claudeLink.selected = (claudeLink.selected + delta % n + n) % n;
  needRedraw = true;
}

static void handleAskButtons() {
  if (upButton().wasClicked())   moveAsk(-1);
  if (downButton().wasClicked()) moveAsk(+1);
  scrollRepeat(upButton(),   -1);
  scrollRepeat(downButton(), +1);

  if (M5.BtnA.wasClicked()) {
    beep(3000, 90);
    claudeLink.answer(claudeLink.selected);
    ui::toast("Wyslano", ui::C_ON);
    needRedraw = true;
  }
  if (M5.BtnA.wasHold()) {          // long press on the big button = dismiss
    claudeLink.dismiss();
    needRedraw = true;
  }
}

static void handleButtons() {
  // Edge-triggered on purpose: a noisy button line held at a steady level
  // must not be able to pin the screen awake forever.
  bool clicked = M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnPWR.wasClicked();
  bool pressed = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnPWR.wasPressed();

  // While the screen is dark the first press only wakes it, so a blind press
  // never toggles a light by accident.
  if (ui::asleep()) {
    if (clicked || pressed) { ui::wake(); needRedraw = true; }
    return;
  }
  if (clicked || pressed) ui::wake();

  if (claudeLink.pending()) { handleAskButtons(); return; }

  if (upButton().wasClicked())   moveCursor(-1);
  if (downButton().wasClicked()) moveCursor(+1);
  scrollRepeat(upButton(),   -1);
  scrollRepeat(downButton(), +1);

  if (M5.BtnA.wasClicked()) {
    if (top().screen == SCR_BUDDY) pushFrame(SCR_INFO, "Szczegoly", "");
    else                           quickOrOpen();
  }
  if (M5.BtnA.wasHold()) {
    // At the root there is nothing to go back to, so that gesture opens
    // settings instead - keeps the main menu down to the two choices.
    if (stack.size() > 1) popFrame();
    else                  pushFrame(SCR_SETTINGS, "Ustawienia", "");
  }
}

// -------------------------------------------------------------- boot flow --

static void runPortal(bool forcePortal) {
  WiFiManager wm;
  WiFiManagerParameter pUrl("haurl", "Adres Home Assistant", settings.haUrl.c_str(), 96);
  WiFiManagerParameter pTok("hatok", "Long-lived access token", "", 220);
  wm.addParameter(&pUrl);
  wm.addParameter(&pTok);
  wm.setTitle("M5 Pilot HA");
  wm.setConfigPortalTimeout(600);
  wm.setAPCallback([](WiFiManager* m) {
    ui::drawPortal(AP_NAME, "http://192.168.4.1");
  });
  wm.setSaveParamsCallback([&]() {
    settings.haUrl = pUrl.getValue();
    String tok = pTok.getValue();
    if (tok.length() > 20) settings.haToken = tok;   // blank field keeps the old token
    settings.save();
  });

  // autoConnect() only opens the portal when no Wi-Fi credentials are stored,
  // so a re-configuration (Wi-Fi fine, HA token missing) must force it open.
  bool ok = forcePortal ? wm.startConfigPortal(AP_NAME) : wm.autoConnect(AP_NAME);
  if (!ok) {
    ui::drawMessage("Brak Wi-Fi", "Nie udalo sie polaczyc. Restartuje.", ui::C_ERR);
    delay(2500);
    ESP.restart();
  }
  settings.haUrl   = pUrl.getValue()[0] ? pUrl.getValue() : settings.haUrl;
  String tok = pTok.getValue();
  if (tok.length() > 20) settings.haToken = tok;
  settings.save();
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  M5.begin(cfg);
  Serial.begin(115200);          // WiFiManager diagnostics land here
  pinMode(4, OUTPUT);           // Plus2 power-hold: keeps us alive off USB
  digitalWrite(4, HIGH);

  M5.Speaker.begin();

  settings.load();
  M5.Speaker.setVolume(settings.volume);
  ui::begin();
  ui::drawMessage("Pilot HA", String("Start... v") + FW_VERSION, ui::C_SEL);

  M5.BtnA.setHoldThresh(600);
  M5.BtnB.setHoldThresh(600);
  M5.BtnPWR.setHoldThresh(600);

  // Holding the side button during boot forces the configuration portal.
  M5.update();
  bool force = M5.BtnB.isPressed();
  if (force) settings.clearHa();

  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);          // DTIM modem sleep between beacons

  if (force) {
    runPortal(true);
  } else if (!settings.configured()) {
    runPortal(false);                                  // no Wi-Fi yet -> portal
    if (!settings.configured()) runPortal(true);       // Wi-Fi ok, HA still blank
  } else {
    WiFiManager wm;
    wm.setEnableConfigPortal(false);
    wm.setConnectTimeout(20);
    if (!wm.autoConnect(AP_NAME)) {
      ui::drawMessage("Brak Wi-Fi", "Przytrzymaj boczny przycisk przy starcie, by skonfigurowac.", ui::C_ERR);
      delay(3000);
    }
  }

  if (WiFi.status() == WL_CONNECTED && !settings.haUrl.length()) {
    String found = discoverHa();
    if (found.length()) { settings.haUrl = found; settings.save(); }
  }

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
  claudeLink.firmware = FW_VERSION;
  claudeLink.build = FW_BUILD;
  claudeLink.begin(HOSTNAME);

  stack.clear();
  Frame home;
  home.screen = SCR_MAIN;
  home.title  = "Pilot HA";
  stack.push_back(home);

  ui::drawMessage("Pilot HA", "Pobieram encje...", ui::C_SEL);
  doRefresh(true);
  if (!ha.online) ui::toast(ha.lastError, ui::C_ERR);
  beepAsk();
  needRedraw = true;
}

void loop() {
  M5.update();
  claudeLink.loop();
  networkWatchdog();
  ui::tick();

  // A freshly arrived question or banner pulls the screen forward.
  if (claudeLink.wake) {
    claudeLink.wake = false;
    ui::wake();
    needRedraw = true;
    if (claudeLink.pending()) beepAsk();
    else if (claudeLink.notifying()) beep(1800, 90);
  }

  handleButtons();

  // Clock follows the screen: full speed only while something is on it.
  bool sleeping = ui::asleep();
  setCpuFast(!sleeping && settings.powerSave != 2);

  if (sleeping) {
    // Nothing to draw and nothing to poll - just service the network slowly.
    delay(40);
    return;
  }

  if (pendingRefreshAt && millis() > pendingRefreshAt) {
    pendingRefreshAt = 0;
    doRefresh(true);
  }
  uint32_t pollEvery = settings.powerSave == 2 ? 15000 : 6000;
  if (millis() - lastPollMs > pollEvery && !claudeLink.pending()) {
    lastPollMs = millis();
    doRefresh(true);
  }

  if (claudeLink.pending()) {
    // Re-announce every 20 s so an unanswered question is not missed.
    static uint32_t lastNag = 0;
    if (millis() - lastNag > 20000) {
      if (lastNag) beepAsk();
      lastNag = millis();
    }

    static uint32_t lastTick = 0;
    if (needRedraw || millis() - lastTick > 500) {
      lastTick = millis();
      ui::drawAsk(claudeLink.title, claudeLink.question, claudeLink.options,
                  claudeLink.selected);
      needRedraw = false;
    }
    delay(10);
    return;
  }

  if (claudeLink.notifying()) {
    uint16_t col = claudeLink.notifyLevel == "error" ? ui::C_ERR :
                   claudeLink.notifyLevel == "done"  ? ui::C_ON  : ui::C_CLAUDE;
    ui::drawBanner(claudeLink.notifyText, col, "CLAUDE CODE");
    if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnPWR.wasClicked())
      claudeLink.notifyUntilMs = 0;
    delay(20);
    needRedraw = true;
    return;
  }

  if (top().screen == SCR_BUDDY) {
    static uint32_t lastBuddy = 0;
    if (needRedraw || millis() - lastBuddy > (settings.powerSave == 2 ? 800u : 400u)) {
      lastBuddy = millis();
      // Aggressive mode halves the animation rate.
      UsageBar bars[2];
      String notes[3];
      int nNotes = 0;
      bool happy = true;
      int nBars = buddyContent(bars, 2, notes, 3, nNotes, happy);
      bool blink = (millis() % 4200) < 160;   // an occasional blink
      // Slow left-right glance; one step every 450 ms, full sweep ~9 s.
      static const int8_t LOOK[] = { 0, 0, 0, 0, 0, 2, 3, 3, 3, 2,
                                     0, 0, 0, 0, 0, -2, -3, -3, -3, -2 };
      int lookX = LOOK[(millis() / 450) % (sizeof(LOOK) / sizeof(LOOK[0]))];
      ui::drawBuddy(bars, nBars, notes, nNotes, ui::C_CLAUDE, blink, happy,
                    lookX, T(S_HINT_BUDDY));
      needRedraw = false;
    }
    delay(10);
    return;
  }

  if (needRedraw || ui::toasting()) {
    rebuild();
    const char* hint = top().screen == SCR_MAIN
                         ? T(S_HINT_MAIN) : T(S_HINT_LIST);
    ui::drawList(top().title.c_str(), items, top().cursor, top().top,
                 ui::visibleRows(), hint);
    needRedraw = false;
  }

  delay(10);
}
