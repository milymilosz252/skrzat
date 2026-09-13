#pragma once
#include <Arduino.h>

// Persisted configuration. Wi-Fi credentials are owned by WiFiManager/NVS,
// everything else lives in our own "hapilot" namespace.
struct Settings {
  String  haUrl;                 // e.g. http://homeassistant.local:8123 (no trailing slash)
  String  haToken;               // Home Assistant long-lived access token
  String  stickKey;              // optional shared key required by /claude/* endpoints
  uint8_t brightness  = 110;     // 0..255 LCD backlight
  uint16_t sleepSec   = 45;      // idle seconds before the screen dims off
  bool     beep       = true;
  uint8_t  volume     = 200;     // buzzer loudness 0-255
  uint8_t  rotation   = 1;       // 1 or 3 - landscape, either way up
  bool     swapUpDown = false;   // flips which side button scrolls up
  uint8_t  powerSave  = 1;       // 0 = off, 1 = normal, 2 = aggressive

  void load();
  void save();
  void clearHa();
  bool configured() const { return haUrl.length() > 0 && haToken.length() > 0; }
};

extern Settings settings;
