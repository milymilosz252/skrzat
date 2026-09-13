#include "ha_client.h"
#include "settings.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "i18n.h"

HaClient ha;

// One REST round-trip gives us every controllable entity together with its
// friendly name, area and state. Rendering a template beats walking
// /api/states because /api/states knows nothing about the area registry.
static const char* LIST_TEMPLATE =
  "{% for s in states if s.domain in ['light','switch','fan','cover','scene','script',"
  "'automation','media_player','input_boolean','lock','climate','vacuum','button',"
  "'humidifier','siren','valve','input_button','todo'] %}"
  "{{s.entity_id}}|{{s.state}}|{{area_name(s.entity_id) or ''}}|{{s.name}}|"
  "{{s.attributes.brightness if s.attributes.brightness is defined else ''}}\n"
  "{% endfor %}";

bool HaClient::request(const char* method, const String& path, const String& body,
                       String& response, int& code) {
  if (WiFi.status() != WL_CONNECTED) { lastError = "Brak Wi-Fi"; return false; }
  if (!settings.configured())        { lastError = "Brak konfiguracji HA"; return false; }

  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(5000);
  http.setReuse(false);
  if (!http.begin(settings.haUrl + path)) { lastError = "Zly adres HA"; return false; }

  http.addHeader("Authorization", "Bearer " + settings.haToken);
  http.addHeader("Content-Type", "application/json");

  if (strcmp(method, "GET") == 0) code = http.GET();
  else                            code = http.sendRequest(method, (uint8_t*)body.c_str(), body.length());

  bool ok = code > 0 && code < 300;
  response = ok ? http.getString() : "";
  if (!ok) {
    if (code == 401)      lastError = "Token odrzucony (401)";
    else if (code > 0)    lastError = "HA HTTP " + String(code);
    else                  lastError = "Brak polaczenia z HA";
  }
  http.end();
  return ok;
}

bool HaClient::refresh() {
  JsonDocument doc;
  doc["template"] = LIST_TEMPLATE;
  String body;
  serializeJson(doc, body);

  String resp;
  int code = 0;
  if (!request("POST", "/api/template", body, resp, code)) { online = false; return false; }

  std::vector<Entity> fresh;
  fresh.reserve(64);

  int pos = 0;
  while (pos < (int)resp.length()) {
    int nl = resp.indexOf('\n', pos);
    if (nl < 0) nl = resp.length();
    String line = resp.substring(pos, nl);
    pos = nl + 1;
    line.trim();
    if (line.length() < 5) continue;

    // entity_id|state|area|name|brightness
    String f[5];
    int start = 0, idx = 0;
    for (; idx < 5; idx++) {
      int bar = line.indexOf('|', start);
      if (idx == 4 || bar < 0) { f[idx] = line.substring(start); break; }
      f[idx] = line.substring(start, bar);
      start = bar + 1;
    }
    if (f[0].indexOf('.') < 0) continue;

    Entity e;
    e.id         = f[0];
    e.domain     = f[0].substring(0, f[0].indexOf('.'));
    e.state      = f[1];
    e.area       = f[2];
    e.name       = f[3].length() ? f[3] : f[0];
    e.brightness = f[4].length() ? f[4].toInt() : -1;
    fresh.push_back(e);
  }

  if (fresh.empty()) { lastError = "HA nie zwrocil encji"; online = false; return false; }

  // Group by domain, then by name, so the "all entities" list is predictable.
  std::sort(fresh.begin(), fresh.end(), [](const Entity& a, const Entity& b) {
    if (a.domain != b.domain) return a.domain < b.domain;
    return a.name < b.name;
  });

  entities.swap(fresh);
  online = true;
  lastError = "";
  lastRefreshMs = millis();
  return true;
}

bool HaClient::refreshOne(const String& entityId) {
  String resp;
  int code = 0;
  if (!request("GET", "/api/states/" + entityId, "", resp, code)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, resp)) { lastError = "Zla odpowiedz HA"; return false; }

  Entity* e = find(entityId);
  if (!e) return false;
  e->state = doc["state"].as<String>();
  if (doc["attributes"]["brightness"].is<int>())
    e->brightness = doc["attributes"]["brightness"].as<int>();
  return true;
}

bool HaClient::callService(const String& domain, const String& service,
                           const String& entityId, const String& extraJson) {
  String body = "{\"entity_id\":\"" + entityId + "\"";
  if (extraJson.length()) body += "," + extraJson;
  body += "}";

  String resp;
  int code = 0;
  return request("POST", "/api/services/" + domain + "/" + service, body, resp, code);
}

bool HaClient::runAction(const Entity& e, const Action& a) {
  String domain = strlen(a.domain) ? String(a.domain) : e.domain;
  String extra;

  // Brightness steps need the current level, which HA does not offer as a delta.
  if (strcmp(a.service, "__bright") == 0) {
    int curPct = e.brightness > 0 ? (e.brightness * 100 + 127) / 255 : (e.isOn() ? 100 : 0);
    int next   = constrain(curPct + a.arg, 5, 100);
    extra      = "\"brightness_pct\":" + String(next);
    return callService(domain, "turn_on", e.id, extra);
  }
  if (a.arg && strcmp(a.service, "turn_on") == 0 && domain == "light") {
    extra = "\"brightness_pct\":" + String(a.arg);
    return callService(domain, "turn_on", e.id, extra);
  }
  return callService(domain, a.service, e.id, extra);
}

Entity* HaClient::find(const String& id) {
  for (auto& e : entities) if (e.id == id) return &e;
  return nullptr;
}

size_t HaClient::countDomain(const char* domain) const {
  size_t n = 0;
  for (const auto& e : entities) if (e.domain == domain) n++;
  return n;
}

std::vector<String> HaClient::areas() const {
  std::vector<String> out;
  for (const auto& e : entities) {
    if (!e.area.length()) continue;
    bool seen = false;
    for (const auto& a : out) if (a == e.area) { seen = true; break; }
    if (!seen) out.push_back(e.area);
  }
  std::sort(out.begin(), out.end());
  return out;
}

int HaClient::actionsFor(const Entity& e, Action* out, int maxOut) {
  int n = 0;
  auto add = [&](const char* l, const char* d, const char* s, int arg) {
    if (n < maxOut) out[n++] = Action{l, d, s, arg};
  };

  if (e.domain == "light") {
    add(T(S_TOGGLE),      "", "toggle",    0);
    add(T(S_BRIGHTER), "", "__bright", +20);
    add(T(S_DIMMER), "", "__bright", -20);
    add(T(S_FULL),     "", "turn_on",  100);
    add(T(S_DIM10),  "", "turn_on",   10);
    add(T(S_TURN_OFF),        "", "turn_off",   0);
  } else if (e.domain == "scene") {
    add(T(S_ACTIVATE), "", "turn_on", 0);
  } else if (e.domain == "script") {
    add(T(S_RUN),       "", "turn_on", 0);
    add(T(S_STOP),     "", "turn_off", 0);
  } else if (e.domain == "button" || e.domain == "input_button") {
    add(T(S_PRESS),      "", "press", 0);
  } else if (e.domain == "automation") {
    add(T(S_RUN_NOW), "", "trigger", 0);
    add(T(S_TURN_ON),         "", "turn_on", 0);
    add(T(S_TURN_OFF),        "", "turn_off", 0);
  } else if (e.domain == "media_player") {
    add(T(S_PLAYPAUSE),  "", "media_play_pause", 0);
    add(T(S_LOUDER),      "", "volume_up",   0);
    add(T(S_QUIETER),        "", "volume_down", 0);
    add(T(S_NEXT),      "", "media_next_track", 0);
    add(T(S_PREV),     "", "media_previous_track", 0);
    add(T(S_MUTE),        "", "volume_mute", 0);
    add(T(S_TURN_OFF),        "", "turn_off", 0);
    add(T(S_TURN_ON),         "", "turn_on",  0);
  } else if (e.domain == "cover" || e.domain == "valve") {
    add(T(S_OPEN),        "", "open_cover",  0);
    add(T(S_CLOSE),       "", "close_cover", 0);
    add("Stop",          "", "stop_cover",  0);
  } else if (e.domain == "lock") {
    add(T(S_LOCK), "", "lock",   0);
    add(T(S_UNLOCK),  "", "unlock", 0);
  } else if (e.domain == "vacuum") {
    add("Start",         "", "start", 0);
    add(T(S_PAUSE),         "", "pause", 0);
    add(T(S_DOCK),       "", "return_to_base", 0);
  } else if (e.domain == "todo") {
    // Read-only from the stick; listing items needs the todo websocket API.
  } else {
    add(T(S_TOGGLE),      "", "toggle",   0);
    add(T(S_TURN_ON),         "", "turn_on",  0);
    add(T(S_TURN_OFF),        "", "turn_off", 0);
  }
  return n;
}

const char* HaClient::domainLabel(const char* domain) {
  String d(domain);
  if (d == "light")         return T(S_LIGHTS);
  if (d == "switch")        return T(S_SWITCHES);
  if (d == "scene")         return T(S_SCENES);
  if (d == "script")        return T(S_SCRIPTS);
  if (d == "automation")    return T(S_AUTOMATIONS);
  if (d == "media_player")  return T(S_MEDIA);
  if (d == "cover")         return "Rolety";
  if (d == "fan")           return "Wentylacja";
  if (d == "lock")          return "Zamki";
  if (d == "climate")       return "Klimat";
  if (d == "vacuum")        return "Odkurzacz";
  if (d == "button")        return "Przyciski";
  if (d == "input_boolean") return T(S_SWITCHES);
  if (d == "input_button")  return "Przyciski";
  if (d == "todo")          return "Listy";
  return domain;
}
