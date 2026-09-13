#include "settings.h"
#include <Preferences.h>
#include <nvs_flash.h>

Settings settings;
static Preferences prefs;
static const char* NS = "hapilot";

void Settings::load() {
  prefs.begin(NS, true);
  haUrl      = prefs.getString("haUrl", "");
  haToken    = prefs.getString("haToken", "");
  stickKey   = prefs.getString("stickKey", "");
  brightness = prefs.getUChar("bright", 110);
  sleepSec   = prefs.getUShort("sleep", 45);
  beep       = prefs.getBool("beep", true);
  volume     = prefs.getUChar("vol", 200);
  rotation   = prefs.getUChar("rot", 1);
  swapUpDown = prefs.getBool("swap", false);
  powerSave  = prefs.getUChar("pwr", 1);
  language   = prefs.getUChar("lang", 0);
  prefs.end();

  // Normalise: strip a trailing slash so we can concatenate paths blindly.
  while (haUrl.endsWith("/")) haUrl.remove(haUrl.length() - 1);
}

void Settings::save() {
  while (haUrl.endsWith("/")) haUrl.remove(haUrl.length() - 1);
  prefs.begin(NS, false);
  prefs.putString("haUrl", haUrl);
  prefs.putString("haToken", haToken);
  prefs.putString("stickKey", stickKey);
  prefs.putUChar("bright", brightness);
  prefs.putUShort("sleep", sleepSec);
  prefs.putBool("beep", beep);
  prefs.putUChar("vol", volume);
  prefs.putUChar("rot", rotation);
  prefs.putBool("swap", swapUpDown);
  prefs.putUChar("pwr", powerSave);
  prefs.putUChar("lang", language);
  prefs.end();
}

// Nuclear option: drop our namespace and let WiFiManager's credentials go
// with it, so the next boot comes up as a factory-fresh device.
void Settings::factoryReset() {
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();
  nvs_flash_erase();
  nvs_flash_init();
}

void Settings::clearHa() {
  prefs.begin(NS, false);
  prefs.remove("haUrl");
  prefs.remove("haToken");
  prefs.end();
  haUrl = "";
  haToken = "";
}
