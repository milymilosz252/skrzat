#include "claude_link.h"
#include "settings.h"
#include "ha_client.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "ui.h"

ClaudeLink claudeLink;
static WebServer server(80);

const char* ClaudeLink::statusName() const {
  switch (status) {
    case ASK_PENDING:   return "pending";
    case ASK_ANSWERED:  return "answered";
    case ASK_DISMISSED: return "dismissed";
    case ASK_TIMEOUT:   return "timeout";
    default:            return "idle";
  }
}

// Optional shared key. Empty key => LAN-open, which is the default because the
// stick only ever exposes its own screen, never Home Assistant credentials.
bool ClaudeLink::authorised() {
  if (!settings.stickKey.length()) return true;
  if (server.header("X-Stick-Key") == settings.stickKey) return true;
  if (server.hasArg("key") && server.arg("key") == settings.stickKey) return true;
  server.send(401, "application/json", "{\"error\":\"bad key\"}");
  return false;
}

void ClaudeLink::routes() {
  server.on("/status", HTTP_GET, [this]() {
    JsonDocument d;
    d["device"]     = hostname;
    d["ip"]         = WiFi.localIP().toString();
    d["rssi"]       = WiFi.RSSI();
    d["battery"]    = M5.Power.getBatteryLevel();
    d["charging"]   = M5.Power.isCharging();
    d["ha_online"]  = ha.online;
    d["ha_error"]   = ha.lastError;
    d["entities"]   = (uint32_t)ha.entities.size();
    d["ask_status"] = statusName();
    d["ask_id"]     = id;
    d["asks"]       = asksServed;
    d["usage_known"] = hasUsage();
    d["uptime_s"]   = millis() / 1000;
    d["reconnects"] = reconnects;
    d["sleeping"]   = ui::asleep();
    d["stage"]      = ui::stageNow();
    d["idle_s"]     = ui::idleMs() / 1000;
    d["sleep_after_s"] = ui::sleepAfterMsNow() / 1000;
    d["btn_a"]      = M5.BtnA.isPressed();
    d["btn_b"]      = M5.BtnB.isPressed();
    d["btn_pwr"]    = M5.BtnPWR.isPressed();
    String out; serializeJson(d, out);
    server.send(200, "application/json", out);
  });

  // POST /claude/ask  {"id","title","question","options":[...],"timeout":300}
  server.on("/claude/ask", HTTP_POST, [this]() {
    if (!authorised()) return;

    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
      server.send(400, "application/json", "{\"error\":\"bad json\"}");
      return;
    }
    if (!d["options"].is<JsonArray>() || d["options"].size() == 0) {
      server.send(400, "application/json", "{\"error\":\"options required\"}");
      return;
    }

    id       = d["id"].is<const char*>() ? d["id"].as<String>() : String(millis(), HEX);
    title    = d["title"].as<String>();
    question = d["question"].as<String>();
    options.clear();
    for (JsonVariant v : d["options"].as<JsonArray>()) {
      if (options.size() >= 8) break;          // more than 8 will not fit the screen
      options.push_back(v.as<String>());
    }

    uint32_t timeoutS = d["timeout"].is<uint32_t>() ? d["timeout"].as<uint32_t>() : 300;
    if (timeoutS < 10)   timeoutS = 10;
    if (timeoutS > 3600) timeoutS = 3600;

    selected   = 0;
    status     = ASK_PENDING;
    deadlineMs = millis() + timeoutS * 1000UL;
    wake       = true;
    asksServed++;

    JsonDocument r;
    r["id"]      = id;
    r["status"]  = "pending";
    r["expires"] = timeoutS;
    String out; serializeJson(r, out);
    server.send(200, "application/json", out);
  });

  // GET /claude/answer?id=...
  server.on("/claude/answer", HTTP_GET, [this]() {
    if (!authorised()) return;
    String want = server.arg("id");

    JsonDocument r;
    if (want.length() && want != id) {
      r["status"] = "unknown";
    } else {
      r["status"] = statusName();
      r["id"]     = id;
      if (status == ASK_ANSWERED) {
        r["index"] = selected;
        r["value"] = (selected >= 0 && selected < (int)options.size()) ? options[selected] : "";
      }
    }
    String out; serializeJson(r, out);
    server.send(200, "application/json", out);
  });

  server.on("/claude/cancel", HTTP_POST, [this]() {
    if (!authorised()) return;
    if (status == ASK_PENDING) { status = ASK_DISMISSED; wake = true; }
    server.send(200, "application/json", "{\"status\":\"cancelled\"}");
  });

  // POST /claude/notify  {"text":"...","level":"info|done|error","seconds":6}
  server.on("/claude/notify", HTTP_POST, [this]() {
    if (!authorised()) return;
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
      server.send(400, "application/json", "{\"error\":\"bad json\"}");
      return;
    }
    notifyText  = d["text"].as<String>();
    notifyLevel = d["level"].is<const char*>() ? d["level"].as<String>() : "info";
    uint32_t s  = d["seconds"].is<uint32_t>() ? d["seconds"].as<uint32_t>() : 6;
    notifyUntilMs = millis() + constrain(s, 2U, 120U) * 1000UL;
    wake = true;
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // POST /claude/usage  {"five_hour_left","five_hour_reset","seven_day_left",
  //                       "seven_day_reset","tokens_today","stale"}
  server.on("/claude/usage", HTTP_POST, [this]() {
    if (!authorised()) return;
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
      server.send(400, "application/json", "{\"error\":\"bad json\"}");
      return;
    }
    usageFiveHourUsed  = d["five_hour_used"].is<int>() ? d["five_hour_used"].as<int>() : -1;
    usageSevenDayUsed  = d["seven_day_used"].is<int>() ? d["seven_day_used"].as<int>() : -1;
    usageFiveHourReset = d["five_hour_reset"].as<uint32_t>();
    usageSevenDayReset = d["seven_day_reset"].as<uint32_t>();
    usageTokensToday   = d["tokens_today"].as<uint64_t>();
    usageStale         = d["stale"].as<bool>();
    usageReceivedMs    = millis();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // POST /update  (multipart/form-data, field "firmware") -> flash + reboot
  server.on("/update", HTTP_POST,
    [this]() {
      if (otaDenied) {
        server.sendHeader("Connection", "close");
        server.send(401, "application/json", "{\"error\":\"bad key\"}");
        return;
      }
      bool ok = !Update.hasError();
      server.sendHeader("Connection", "close");
      server.send(ok ? 200 : 500, "application/json",
                  ok ? "{\"status\":\"ok\",\"rebooting\":true}"
                     : "{\"error\":\"update failed\"}");
      if (ok) {
        ui::drawProgress("Gotowe - restart", 100);
        delay(600);
        ESP.restart();
      } else {
        ui::drawMessage("Blad aktualizacji", "Firmware odrzucony.", ui::C_ERR);
      }
    },
    [this]() {
      HTTPUpload& up = server.upload();
      switch (up.status) {
        case UPLOAD_FILE_START: {
          otaDenied = settings.stickKey.length() &&
                      server.header("X-Stick-Key") != settings.stickKey;
          if (otaDenied) return;
          otaBytes = 0;
          otaTotal = server.header("X-Firmware-Size").toInt();
          Update.begin(UPDATE_SIZE_UNKNOWN);
          ui::wake();
          ui::drawProgress("Aktualizacja firmware", 0);
          break;
        }
        case UPLOAD_FILE_WRITE: {
          if (otaDenied) return;
          Update.write(up.buf, up.currentSize);
          uint32_t before = otaBytes;
          otaBytes += up.currentSize;
          // Redraw roughly every 64 KB - cheap enough not to stall the upload.
          if (otaTotal && (otaBytes >> 16) != (before >> 16))
            ui::drawProgress("Aktualizacja firmware", otaBytes * 100 / otaTotal);
          break;
        }
        case UPLOAD_FILE_END:
          if (otaDenied) return;
          Update.end(true);
          break;
        default:
          break;
      }
    });

  server.onNotFound([]() {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
  });
}

void ClaudeLink::begin(const String& host) {
  hostname = host;
  routes();
  const char* headers[] = { "X-Stick-Key", "X-Firmware-Size" };
  server.collectHeaders(headers, 2);
  server.begin();

  if (MDNS.begin(hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("claudestick", "tcp", 80);
  }
}

// After a reconnect the old mDNS registration is dead; re-announce so
// claude-stick.local keeps resolving without a reboot.
void ClaudeLink::restartMdns() {
  MDNS.end();
  if (MDNS.begin(hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("claudestick", "tcp", 80);
  }
}

void ClaudeLink::loop() {
  server.handleClient();
  if (status == ASK_PENDING && millis() > deadlineMs) {
    status = ASK_TIMEOUT;
    wake = true;
  }
}

void ClaudeLink::answer(int index) {
  if (status != ASK_PENDING) return;
  selected = constrain(index, 0, (int)options.size() - 1);
  status   = ASK_ANSWERED;
}

void ClaudeLink::dismiss() {
  if (status == ASK_PENDING) status = ASK_DISMISSED;
}
