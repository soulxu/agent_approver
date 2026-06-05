#include "app_prefs.h"

#include <Preferences.h>

namespace app_prefs {

namespace {
constexpr const char* NS = "aaprefs";
constexpr const char* KEY_URL    = "url";
constexpr const char* KEY_BRIGHT = "bri";

String  g_url;
uint8_t g_bright = 128;
bool    g_loaded = false;

void load() {
  if (g_loaded) return;
  Preferences p;
  if (p.begin(NS, true /*ro*/)) {
    g_url    = p.getString(KEY_URL, "");
    g_bright = p.getUChar(KEY_BRIGHT, 128);
    p.end();
  }
  g_loaded = true;
}
}  // namespace

void begin() { load(); }

String relayUrl() {
  load();
  return g_url;
}

void setRelayUrl(const String& url) {
  load();
  String u = url;
  u.trim();
  while (u.length() > 0 && u[u.length() - 1] == '/') u.remove(u.length() - 1);
  g_url = u;
  Preferences p;
  if (p.begin(NS, false)) { p.putString(KEY_URL, u); p.end(); }
}

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

}  // namespace app_prefs
