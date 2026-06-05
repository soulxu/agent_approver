#include "web_config.h"

#include <WebServer.h>
#include <WiFi.h>

#include "app_prefs.h"
#include "wifi_store.h"

namespace web_config {

namespace {

WebServer* g_srv         = nullptr;
bool       g_softApOn    = false;
String     g_softApSsid;
bool       g_newCredFlag = false;
bool       g_rebootFlag  = false;
uint32_t   g_lastScanMs  = 0;

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

String buildIndex() {
  String html;
  html.reserve(4096);
  html += F("<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Agent Approver</title>"
            "<style>"
            "body{font-family:-apple-system,system-ui,Roboto,sans-serif;"
            "max-width:520px;margin:16px auto;padding:0 12px;color:#222;background:#fafafa;}"
            "h1{font-size:20px;margin:8px 0;} h2{font-size:15px;margin:18px 0 6px;color:#666;}"
            "form{margin:0;} input,button{font-size:15px;}"
            "input[type=text],input[type=password]{width:100%;padding:8px;"
            "border:1px solid #ccc;border-radius:6px;box-sizing:border-box;margin:4px 0;}"
            "button{padding:8px 14px;border:0;border-radius:6px;background:#1976d2;"
            "color:#fff;cursor:pointer;margin:4px 4px 4px 0;}"
            "button.danger{background:#c62828;} button.ghost{background:#eee;color:#222;}"
            "ul{list-style:none;padding:0;} li{padding:8px 10px;background:#fff;"
            "border:1px solid #eee;border-radius:6px;margin:6px 0;display:flex;"
            "justify-content:space-between;align-items:center;gap:8px;}"
            ".ssid{font-weight:600;} .rssi{color:#888;font-size:12px;}"
            ".badge{font-size:11px;background:#e3f2fd;color:#1565c0;"
            "padding:2px 6px;border-radius:4px;margin-left:6px;}"
            ".muted{color:#888;font-size:13px;}"
            "</style></head><body>");
  html += F("<h1>Agent Approver</h1>");

  html += F("<div class=\"muted\">");
  if (WiFi.status() == WL_CONNECTED) {
    html += F("STA \u5df2\u8fde\u63a5 ");
    html += htmlEscape(WiFi.SSID());
    html += F(" (IP ");
    html += WiFi.localIP().toString();
    html += F(", RSSI ");
    html += String(WiFi.RSSI());
    html += F(" dBm)");
  } else {
    html += F("STA \u672a\u8fde\u63a5");
  }
  if (g_softApOn) {
    html += F(" \u00b7 SoftAP: ");
    html += htmlEscape(g_softApSsid);
    html += F(" (192.168.4.1)");
  }
  html += F("</div>");

  // relay URL
  html += F("<h2>Relay \u5730\u5740 (Mac \u4e0a relay.py)</h2>"
           "<form method=\"post\" action=\"/relay\">"
           "<input type=\"text\" name=\"url\" placeholder=\"http://192.168.x.x:8799\" value=\"");
  html += htmlEscape(app_prefs::relayUrl());
  html += F("\"><button type=\"submit\">\u4fdd\u5b58 Relay</button></form>"
           "<div class=\"muted\">\u8fd0\u884c relay.py \u540e\u7ec8\u7aef\u4f1a\u6253\u5370 "
           "\"StickS3 relay URL -> http://...\", \u586b\u8fdb\u6765\u5373\u53ef.</div>");

  // 已保存 WiFi
  auto saved = wifi_store::loadAll();
  html += F("<h2>\u5df2\u4fdd\u5b58\u7f51\u7edc</h2>");
  if (saved.empty()) {
    html += F("<div class=\"muted\">\u8fd8\u6ca1\u4fdd\u5b58. \u4e0b\u9762\u6dfb\u52a0\u4e00\u4e2a.</div>");
  } else {
    html += F("<ul>");
    for (auto& c : saved) {
      html += F("<li><span class=\"ssid\">");
      html += htmlEscape(c.ssid);
      html += F("</span>"
                "<form method=\"post\" action=\"/delete\" "
                "onsubmit=\"return confirm('\u5220\u9664 ' + this.ssid.value + ' ?')\">"
                "<input type=\"hidden\" name=\"ssid\" value=\"");
      html += htmlEscape(c.ssid);
      html += F("\"><button class=\"danger\" type=\"submit\">\u5220\u9664</button></form></li>");
    }
    html += F("</ul>");
  }

  // 添加 WiFi
  html += F("<h2>\u6dfb\u52a0 / \u66f4\u65b0 WiFi</h2>"
           "<form method=\"post\" action=\"/add\">"
           "<input type=\"text\" name=\"ssid\" placeholder=\"SSID\" required>"
           "<input type=\"password\" name=\"password\" placeholder=\"\u5bc6\u7801 (\u5f00\u653e\u7f51\u7edc\u7559\u7a7a)\">"
           "<button type=\"submit\">\u4fdd\u5b58</button></form>");

  // 扫描结果
  int n = WiFi.scanComplete();
  html += F("<h2>\u626b\u63cf\u5230\u7684\u7f51\u7edc</h2>");
  html += F("<form method=\"get\" action=\"/scan\" style=\"display:inline\"><button class=\"ghost\" type=\"submit\">\u91cd\u65b0\u626b\u63cf</button></form>");
  if (n == WIFI_SCAN_RUNNING) {
    html += F("<div class=\"muted\">\u626b\u63cf\u4e2d... \u51e0\u79d2\u540e\u5237\u65b0\u9875\u9762</div>");
  } else if (n <= 0) {
    html += F("<div class=\"muted\">\u70b9\u4e0a\u9762\u6309\u94ae\u5f00\u59cb\u626b\u63cf</div>");
  } else {
    html += F("<ul>");
    for (int i = 0; i < n; ++i) {
      String s = WiFi.SSID(i);
      int32_t r = WiFi.RSSI(i);
      bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      html += F("<li><span><span class=\"ssid\">");
      html += htmlEscape(s);
      html += F("</span>");
      if (open) html += F("<span class=\"badge\">OPEN</span>");
      html += F(" <span class=\"rssi\">");
      html += String(r);
      html += F(" dBm</span></span>"
                "<form method=\"post\" action=\"/add\" style=\"display:flex;gap:4px\">"
                "<input type=\"hidden\" name=\"ssid\" value=\"");
      html += htmlEscape(s);
      html += F("\"><input type=\"password\" name=\"password\" placeholder=\"\u5bc6\u7801\" style=\"width:120px\">"
                "<button type=\"submit\">\u52a0</button></form></li>");
    }
    html += F("</ul>");
  }

  html += F("<h2>\u5176\u5b83</h2>"
           "<form method=\"post\" action=\"/reboot\" "
           "onsubmit=\"return confirm('\u91cd\u542f\u8bbe\u5907?')\">"
           "<button class=\"danger\" type=\"submit\">\u91cd\u542f\u8bbe\u5907</button></form>");

  html += F("</body></html>");
  return html;
}

void handleRoot() {
  g_srv->send(200, "text/html; charset=utf-8", buildIndex());
}

void handleAdd() {
  if (!g_srv->hasArg("ssid")) {
    g_srv->send(400, "text/plain", "missing ssid");
    return;
  }
  String s = g_srv->arg("ssid");
  String p = g_srv->arg("password");
  if (wifi_store::addOrUpdate(s, p)) {
    g_newCredFlag = true;
    Serial.printf("[web] saved cred for '%s'\n", s.c_str());
    g_srv->sendHeader("Location", "/", true);
    g_srv->send(303, "text/plain", "saved");
  } else {
    g_srv->send(500, "text/plain", "save failed");
  }
}

void handleDelete() {
  if (!g_srv->hasArg("ssid")) {
    g_srv->send(400, "text/plain", "missing ssid");
    return;
  }
  wifi_store::remove(g_srv->arg("ssid"));
  g_srv->sendHeader("Location", "/", true);
  g_srv->send(303, "text/plain", "deleted");
}

void handleScan() {
  int n = WiFi.scanComplete();
  if (n != WIFI_SCAN_RUNNING && (millis() - g_lastScanMs > 2000)) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true /*async*/, true /*hidden*/);
    g_lastScanMs = millis();
  }
  g_srv->sendHeader("Location", "/", true);
  g_srv->send(303, "text/plain", "scanning");
}

void handleRelay() {
  String url = g_srv->hasArg("url") ? g_srv->arg("url") : String();
  url.trim();
  if (url.length() > 200) url = url.substring(0, 200);
  app_prefs::setRelayUrl(url);
  Serial.printf("[web] saved relay url='%s'\n", app_prefs::relayUrl().c_str());
  g_srv->sendHeader("Location", "/", true);
  g_srv->send(303, "text/plain", "saved");
}

void handleReboot() {
  g_rebootFlag = true;
  g_srv->send(200, "text/html; charset=utf-8",
              F("<meta charset=\"utf-8\"><body style=\"font-family:sans-serif\">"
                "\u6b63\u5728\u91cd\u542f...\u51e0\u79d2\u540e\u5237\u65b0</body>"));
}

}  // namespace

void begin(bool withSoftAp) {
  if (g_srv) return;

  if (withSoftAp) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "Approver-%02X%02X", mac[4], mac[5]);
    g_softApSsid = buf;
    WiFi.mode(WIFI_AP_STA);
    bool ok = WiFi.softAP(g_softApSsid.c_str());
    g_softApOn = ok;
    Serial.printf("[web] SoftAP=%s ok=%d ip=%s\n",
                  g_softApSsid.c_str(), ok ? 1 : 0,
                  WiFi.softAPIP().toString().c_str());
  }

  g_srv = new WebServer(80);
  g_srv->on("/",       HTTP_GET,  handleRoot);
  g_srv->on("/add",    HTTP_POST, handleAdd);
  g_srv->on("/delete", HTTP_POST, handleDelete);
  g_srv->on("/scan",   HTTP_GET,  handleScan);
  g_srv->on("/relay",  HTTP_POST, handleRelay);
  g_srv->on("/reboot", HTTP_POST, handleReboot);
  WiFi.scanNetworks(true /*async*/, true /*hidden*/);
  g_lastScanMs = millis();
  g_srv->begin();
  Serial.println("[web] http server started on :80");
}

void loop() {
  if (g_srv) g_srv->handleClient();
}

void end() {
  if (g_srv) {
    g_srv->stop();
    delete g_srv;
    g_srv = nullptr;
  }
  if (g_softApOn) {
    WiFi.softAPdisconnect(true);
    g_softApOn = false;
    g_softApSsid = "";
    Serial.println("[web] SoftAP off");
  }
}

bool isRunning()   { return g_srv != nullptr; }
bool isSoftApOn()  { return g_softApOn; }
String softApSsid(){ return g_softApSsid; }

bool consumeNewCredFlag() {
  bool v = g_newCredFlag;
  g_newCredFlag = false;
  return v;
}

bool consumeRebootFlag() {
  bool v = g_rebootFlag;
  g_rebootFlag = false;
  return v;
}

}  // namespace web_config
