#include "net.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>

namespace net {

PollResult poll(const String& baseUrl, int knownVersion, int waitSec,
                uint32_t timeoutMs) {
  PollResult res;
  if (baseUrl.length() == 0) return res;

  String url = baseUrl + "/stick/poll?v=" + String(knownVersion) +
               "&wait=" + String(waitSec);

  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(5000);
  http.setTimeout(timeoutMs);   // read 超时, 必须比 waitSec 大
  if (!http.begin(client, url)) {
    Serial.println("[net] poll begin() failed");
    return res;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[net] poll http %d\n", code);
    http.end();
    return res;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[net] poll json err: %s\n", err.c_str());
    return res;
  }

  res.ok = true;
  res.version = doc["version"] | 0;

  JsonObject ap = doc["approval"].as<JsonObject>();
  if (!ap.isNull()) {
    res.approval.valid     = true;
    res.approval.id        = (const char*)(ap["id"]     | "");
    res.approval.agent     = (const char*)(ap["agent"]  | "");
    res.approval.tool      = (const char*)(ap["tool"]   | "");
    res.approval.title     = (const char*)(ap["title"]  | "");
    res.approval.detail    = (const char*)(ap["detail"] | "");
    res.approval.cwd       = (const char*)(ap["cwd"]    | "");
    res.approval.count     = ap["count"]      | 1;
    res.approval.ageMs     = ap["age_ms"]     | 0;
    res.approval.timeoutMs = ap["timeout_ms"] | 0;
  }

  JsonObject act = doc["activity"].as<JsonObject>();
  if (!act.isNull()) {
    res.activity.valid = true;
    res.activity.agent = (const char*)(act["agent"] | "");
    res.activity.kind  = (const char*)(act["kind"]  | "");
    res.activity.text  = (const char*)(act["text"]  | "");
  }
  return res;
}

bool decide(const String& baseUrl, const String& id, const String& decision,
            uint32_t timeoutMs) {
  if (baseUrl.length() == 0 || id.length() == 0) return false;

  String url = baseUrl + "/stick/decide";
  String payload = String("{\"id\":\"") + id + "\",\"decision\":\"" + decision + "\"}";

  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(4000);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  bool ok = (code == HTTP_CODE_OK);
  if (!ok) Serial.printf("[net] decide http %d\n", code);
  http.end();
  return ok;
}

}  // namespace net
