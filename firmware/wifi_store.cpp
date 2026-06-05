#include "wifi_store.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <algorithm>

namespace wifi_store {

namespace {
constexpr const char* NS  = "aawifi";
constexpr const char* KEY = "json";

bool readJson(String& out) {
  Preferences p;
  if (!p.begin(NS, true /*readonly*/)) {
    out = "";
    return false;
  }
  out = p.getString(KEY, "");
  p.end();
  return true;
}

bool writeJson(const String& body) {
  Preferences p;
  if (!p.begin(NS, false /*rw*/)) {
    Serial.println("[wifi_store] prefs.begin(rw) failed");
    return false;
  }
  size_t n = p.putString(KEY, body);
  p.end();
  if (n == 0 && body.length() > 0) {
    Serial.println("[wifi_store] prefs.putString returned 0");
    return false;
  }
  return true;
}

String serialize(const std::vector<Cred>& creds) {
  JsonDocument doc;
  JsonArray arr = doc["creds"].to<JsonArray>();
  for (const auto& c : creds) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"]     = c.ssid;
    o["password"] = c.password;
  }
  String out;
  serializeJson(doc, out);
  return out;
}
}  // namespace

std::vector<Cred> loadAll() {
  std::vector<Cred> out;
  String body;
  if (!readJson(body) || body.length() == 0) return out;
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[wifi_store] json parse fail, returning empty");
    return out;
  }
  JsonArray arr = doc["creds"].as<JsonArray>();
  for (JsonObject o : arr) {
    const char* s = o["ssid"]     | (const char*)nullptr;
    const char* p = o["password"] | (const char*)nullptr;
    if (!s || !*s) continue;
    Cred c;
    c.ssid     = s;
    c.password = p ? String(p) : String();
    out.push_back(std::move(c));
  }
  return out;
}

bool addOrUpdate(const String& ssid, const String& password) {
  if (ssid.length() == 0) return false;
  auto creds = loadAll();
  creds.erase(std::remove_if(creds.begin(), creds.end(),
              [&](const Cred& c) { return c.ssid == ssid; }),
              creds.end());
  Cred fresh;
  fresh.ssid     = ssid;
  fresh.password = password;
  creds.insert(creds.begin(), std::move(fresh));
  if (creds.size() > MAX_CREDS) creds.resize(MAX_CREDS);
  return writeJson(serialize(creds));
}

bool remove(const String& ssid) {
  auto creds = loadAll();
  auto it = std::remove_if(creds.begin(), creds.end(),
              [&](const Cred& c) { return c.ssid == ssid; });
  if (it == creds.end()) return true;
  creds.erase(it, creds.end());
  return writeJson(serialize(creds));
}

bool clearAll() {
  return writeJson("");
}

}  // namespace wifi_store
