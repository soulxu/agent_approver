#include "app_prefs.h"

#include <Preferences.h>

namespace app_prefs {

namespace {
constexpr const char* NS = "aaprefs";
constexpr const char* KEY_BRIGHT = "bri";
constexpr const char* KEY_BUZZ = "buzz";

uint8_t g_bright = 128;
bool    g_buzz = false;
bool    g_loaded = false;

void load() {
  if (g_loaded) return;
  Preferences p;
  if (p.begin(NS, true /*ro*/)) {
    g_bright = p.getUChar(KEY_BRIGHT, 128);
    g_buzz = p.getBool(KEY_BUZZ, false);
    p.end();
  }
  g_loaded = true;
}
}  // namespace

void begin() { load(); }

uint8_t brightness() {
  load();
  return g_bright;
}

void setBrightness(uint8_t v) {
  load();
  g_bright = v;
  Preferences p;
  if (p.begin(NS, false)) { p.putUChar(KEY_BRIGHT, v); p.end(); }
}

bool soundBuzz() {
  load();
  return g_buzz;
}

void setSoundBuzz(bool v) {
  load();
  g_buzz = v;
  Preferences p;
  if (p.begin(NS, false)) { p.putBool(KEY_BUZZ, v); p.end(); }
}

}  // namespace app_prefs
