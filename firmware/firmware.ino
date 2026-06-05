// ============================================================================
// agent_approver - M5StickS3 上的 "AI agent 审批 + 活动" 终端
//
// 板子: M5Stack StickS3 (ESP32-S3-PICO, 8MB Flash + 8MB PSRAM)
// 屏幕: ST7789P3 135x240, rotation=1 横屏 240x135
// 按键: BtnA = 正面 M5 键, BtnB = 侧面键
//
// 干什么:
//   Mac 上跑 relay.py, Cursor/Claude/Codex 的 hook 把 "要批准的操作" 和 "正在
//   做的事" 推给 relay. StickS3 用 long-poll 拉过来:
//     - 有待审批 -> 大屏显示命令, BtnA 批准 / BtnB 拒绝, 结果回传 relay -> agent.
//     - 没待审批 -> 显示 agent 最近在忙什么 (编辑/运行/任务...).
//
// 配网: WiFi 凭据存 NVS. 没连上 -> SoftAP "Approver-XXXX" + http://192.168.4.1.
//       连上后 http://<ip>/ 也能配置, 在里面填 relay URL.
//       长按 BtnB 强制开/关 SoftAP 配网.
// ============================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <set>
#include <vector>

#include "app_prefs.h"
#include "net.h"
#include "web_config.h"
#include "wifi_store.h"

// HTTP + ArduinoJson 在 loop task 上跑, 多给点栈.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;

static M5Canvas canvas(&M5.Display);

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

// 调色板
static const uint16_t COL_BG      = rgb565(8, 10, 16);
static const uint16_t COL_FG      = rgb565(235, 238, 245);
static const uint16_t COL_DIM     = rgb565(120, 130, 150);
static const uint16_t COL_AMBER   = rgb565(255, 186, 64);
static const uint16_t COL_GREEN   = rgb565(60, 200, 110);
static const uint16_t COL_RED     = rgb565(235, 70, 70);
static const uint16_t COL_BLUE    = rgb565(70, 160, 255);
static const uint16_t COL_PANEL   = rgb565(20, 24, 34);

// ---------------- 状态 ----------------
static std::vector<wifi_store::Cred> g_savedCreds;
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 20000;
static uint32_t g_lastWifiRetryMs  = 0;
static size_t   g_nextRetryCredIdx = 0;
static bool     g_userForcedAp     = false;

static int           g_version    = 0;
static net::Approval g_approval;
static net::Activity g_activity;
static uint32_t g_lastPollMs   = 0;
static uint32_t g_lastPollOkMs = 0;
static String   g_toast;
static uint32_t g_toastUntil   = 0;

static const uint8_t BRIGHT_LEVELS[] = {64, 128, 200};
static int g_brightIdx = 1;

// 按键 (level-based, 容忍 long-poll 期间漏掉的边沿)
static uint32_t g_btnAAt = 0; static bool g_btnAHold = false;
static uint32_t g_btnBAt = 0; static bool g_btnBHold = false;
static constexpr uint32_t BTN_HOLD_MS = 800;

// =============================================================================
// WiFi (沿用 stick_s3_eyes 那套)
// =============================================================================
static void enableWifiPowerSave() {
  if (WiFi.getSleep()) return;
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
}

static bool tryConnect(const wifi_store::Cred& c, uint32_t timeoutMs) {
  Serial.printf("[wifi] try '%s' ...\n", c.ssid.c_str());
  WiFi.disconnect(false, true);
  if (c.password.length() == 0) WiFi.begin(c.ssid.c_str());
  else                          WiFi.begin(c.ssid.c_str(), c.password.c_str());
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      Serial.printf("[wifi] connected '%s' ip=%s\n", c.ssid.c_str(),
                    WiFi.localIP().toString().c_str());
      enableWifiPowerSave();
      return true;
    }
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) return false;
    delay(80);
  }
  return false;
}

static void bootConnectWifi() {
  g_savedCreds = wifi_store::loadAll();
  if (g_savedCreds.empty()) {
    Serial.println("[boot] no saved wifi, will start SoftAP");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, true);
  int n = WiFi.scanNetworks(false, false);
  std::set<String> visible;
  for (int i = 0; i < n; ++i) visible.insert(WiFi.SSID(i));
  WiFi.scanDelete();
  for (const auto& c : g_savedCreds) {
    if (visible.count(c.ssid) == 0) continue;
    if (tryConnect(c, 8000)) return;
  }
  Serial.println("[boot] no saved wifi reachable now");
}

static void maybeRetryWifi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) { g_nextRetryCredIdx = 0; return; }
  if (web_config::isSoftApOn()) return;
  if (g_savedCreds.empty()) return;
  if (now - g_lastWifiRetryMs < WIFI_RETRY_INTERVAL_MS) return;
  g_lastWifiRetryMs = now;
  size_t idx = g_nextRetryCredIdx % g_savedCreds.size();
  g_nextRetryCredIdx++;
  const auto& c = g_savedCreds[idx];
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (c.password.length() == 0) WiFi.begin(c.ssid.c_str());
  else                          WiFi.begin(c.ssid.c_str(), c.password.c_str());
}

static void onWebSavedNewCred() {
  g_savedCreds = wifi_store::loadAll();
  g_nextRetryCredIdx = 0;
  g_lastWifiRetryMs  = 0;
}

// =============================================================================
// 渲染工具
// =============================================================================
static int utf8Len(uint8_t c) {
  if ((c & 0x80) == 0)    return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// 按当前字体把 s 折行绘制. 返回实际画了几行. 超出 maxLines 末尾画省略号.
static int drawWrapped(const String& s, int x, int y, int maxW, int lineH,
                       int maxLines, uint16_t color) {
  canvas.setTextColor(color);
  canvas.setTextDatum(TL_DATUM);
  int line = 0;
  size_t i = 0;
  String cur = "";
  while (i < s.length()) {
    int len = utf8Len((uint8_t)s[i]);
    if (i + len > s.length()) len = s.length() - i;
    String g = s.substring(i, i + len);
    i += len;
    if (g == "\n") {
      canvas.drawString(cur, x, y + line * lineH);
      cur = "";
      line++;
      if (line >= maxLines) break;
      continue;
    }
    if (g == "\r") continue;
    String trial = cur + g;
    if (canvas.textWidth(trial) > maxW && cur.length() > 0) {
      canvas.drawString(cur, x, y + line * lineH);
      cur = g;
      line++;
      if (line >= maxLines) { cur = ""; break; }
    } else {
      cur = trial;
    }
  }
  if (line < maxLines && cur.length() > 0) {
    canvas.drawString(cur, x, y + line * lineH);
    line++;
  } else if (i < s.length()) {
    // 还有没画完的内容: 在最后一行尾巴打个省略号
    canvas.drawString("...", x + maxW - 18, y + (maxLines - 1) * lineH);
  }
  return line;
}

static void applyBrightness() {
  M5.Display.setBrightness(BRIGHT_LEVELS[g_brightIdx]);
}

static void setToast(const String& t) {
  g_toast = t;
  g_toastUntil = millis() + 1200;
}

// =============================================================================
// 屏幕
// =============================================================================
static void renderConfig() {
  canvas.fillScreen(COL_BG);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(COL_AMBER);
  canvas.drawString("\u914d\u7f51\u6a21\u5f0f", 8, 6);  // 配网模式

  canvas.setFont(&fonts::efontCN_16);
  if (web_config::isSoftApOn()) {
    canvas.setTextColor(COL_FG);
    canvas.drawString("AP: " + web_config::softApSsid(), 8, 34);
    canvas.setTextColor(COL_DIM);
    drawWrapped("\u8fde\u4e0a\u540e\u6d4f\u89c8\u5668\u6253\u5f00 192.168.4.1 \u914d WiFi",
                8, 58, SCREEN_W - 16, 18, 2, COL_DIM);  // 连上后浏览器打开...
  } else {
    canvas.setTextColor(COL_FG);
    drawWrapped("\u6b63\u5728\u8fde\u63a5 WiFi...", 8, 40, SCREEN_W - 16, 18, 2, COL_FG);
  }
  canvas.setTextColor(COL_DIM);
  canvas.drawString("\u957f\u6309 B \u5207\u6362\u914d\u7f51", 8, SCREEN_H - 22);  // 长按 B 切换配网
  canvas.pushSprite(0, 0);
}

static void renderIdle() {
  canvas.fillScreen(COL_BG);

  // 顶栏
  canvas.fillRect(0, 0, SCREEN_W, 22, COL_PANEL);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(COL_BLUE);
  canvas.drawString("Agent Approver", 8, 3);

  bool relayOk = (millis() - g_lastPollOkMs) < 30000 && g_lastPollOkMs != 0;
  canvas.setTextDatum(TR_DATUM);
  canvas.setTextColor(relayOk ? COL_GREEN : COL_RED);
  canvas.drawString(relayOk ? "relay \u00b7" : "relay \u00d7", SCREEN_W - 6, 3);

  // 活动区
  canvas.setFont(&fonts::efontCN_16);
  if (g_activity.valid && g_activity.text.length() > 0) {
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(COL_AMBER);
    String head = "[" + g_activity.agent + "]";
    canvas.drawString(head, 8, 30);
    drawWrapped(g_activity.text, 8, 52, SCREEN_W - 16, 19, 4, COL_FG);
  } else {
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(COL_DIM);
    canvas.drawString("\u7b49\u5f85 agent...", SCREEN_W / 2, SCREEN_H / 2);  // 等待 agent...
  }

  // 底栏
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(BL_DATUM);
  canvas.setTextColor(COL_DIM);
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("no wifi");
  canvas.drawString(ip, 8, SCREEN_H - 4);

  if (g_toast.length() > 0 && millis() < g_toastUntil) {
    canvas.setTextDatum(BR_DATUM);
    canvas.setTextColor(COL_GREEN);
    canvas.drawString(g_toast, SCREEN_W - 6, SCREEN_H - 4);
  }
  canvas.pushSprite(0, 0);
}

static void renderApproval() {
  canvas.fillScreen(COL_BG);
  const net::Approval& a = g_approval;

  // 顶栏: [agent] tool   (i/n)
  canvas.fillRect(0, 0, SCREEN_W, 20, rgb565(60, 40, 0));
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(COL_AMBER);
  String head = "[" + a.agent + "] " + a.tool;
  canvas.drawString(head, 6, 2);
  if (a.count > 1) {
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString("x" + String(a.count), SCREEN_W - 6, 2);
  }

  // 倒计时条
  if (a.timeoutMs > 0) {
    float frac = 1.0f - (float)a.ageMs / (float)a.timeoutMs;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int w = (int)(SCREEN_W * frac);
    canvas.fillRect(0, 20, w, 3, COL_AMBER);
  }

  // 标题 (命令)
  canvas.setFont(&fonts::efontCN_16);
  int titleLines = drawWrapped(a.title, 6, 28, SCREEN_W - 12, 19, 2, COL_FG);

  // 详情 (灰, 小字)
  int detailY = 28 + titleLines * 19 + 2;
  if (a.detail.length() > 0 && a.detail != a.title && detailY < SCREEN_H - 30) {
    canvas.setFont(&fonts::efontCN_14);
    int avail = (SCREEN_H - 30 - detailY) / 15;
    if (avail > 0) drawWrapped(a.detail, 6, detailY, SCREEN_W - 12, 15, avail, COL_DIM);
  }

  // 底部两个按键
  int by = SCREEN_H - 26;
  canvas.fillRoundRect(4, by, SCREEN_W / 2 - 8, 24, 5, rgb565(20, 70, 35));
  canvas.fillRoundRect(SCREEN_W / 2 + 4, by, SCREEN_W / 2 - 8, 24, 5, rgb565(80, 25, 25));
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(COL_GREEN);
  canvas.drawString("A \u00b7 \u6279\u51c6", SCREEN_W / 4, by + 12);          // A · 批准
  canvas.setTextColor(COL_RED);
  canvas.drawString("B \u00b7 \u62d2\u7edd", SCREEN_W * 3 / 4, by + 12);      // B · 拒绝
  canvas.pushSprite(0, 0);
}

static void renderFrame() {
  if (web_config::isSoftApOn() || WiFi.status() != WL_CONNECTED) {
    renderConfig();
  } else if (g_approval.valid) {
    renderApproval();
  } else {
    renderIdle();
  }
}

// =============================================================================
// 轮询
// =============================================================================
static void doPoll(const String& relay, int waitSec, uint32_t timeoutMs) {
  g_lastPollMs = millis();
  net::PollResult r = net::poll(relay, g_version, waitSec, timeoutMs);
  if (!r.ok) return;
  g_lastPollOkMs = millis();
  g_version = r.version;
  if (r.activity.valid) g_activity = r.activity;

  bool wasValid = g_approval.valid;
  g_approval = r.approval;
  if (g_approval.valid && !wasValid) {
    // 新审批到达: 拉满亮度引起注意
    M5.Display.setBrightness(255);
    Serial.printf("[poll] NEW approval %s :: %s\n",
                  g_approval.id.c_str(), g_approval.title.c_str());
  } else if (!g_approval.valid && wasValid) {
    applyBrightness();   // 审批消失 (被决定 / relay 超时), 恢复亮度
  }
}

static void sendDecision(const char* decision) {
  if (!g_approval.valid) return;
  String relay = app_prefs::relayUrl();
  String id = g_approval.id;
  bool ok = net::decide(relay, id, decision, 6000);
  Serial.printf("[btn] decide %s ok=%d\n", decision, ok ? 1 : 0);
  setToast(ok ? (String(decision) == "allow" ? "\u5df2\u6279\u51c6" : "\u5df2\u62d2\u7edd")  // 已批准 / 已拒绝
              : "\u53d1\u9001\u5931\u8d25");                                                   // 发送失败
  if (ok) {
    g_approval = net::Approval();   // 清空, 立即重新拉取
    applyBrightness();
    g_lastPollMs = 0;
  }
}

// =============================================================================
// 按键 (level-based)
// =============================================================================
static void handleButtons(uint32_t now) {
  // ---- BtnA ----
  if (M5.BtnA.isPressed()) {
    if (g_btnAAt == 0) { g_btnAAt = now; g_btnAHold = false; }
  } else {
    if (g_btnAAt != 0 && !g_btnAHold) {
      if (g_approval.valid) {
        sendDecision("allow");
      } else {
        g_brightIdx = (g_brightIdx + 1) % (int)(sizeof(BRIGHT_LEVELS));
        applyBrightness();
      }
    }
    g_btnAAt = 0;
  }

  // ---- BtnB ----
  if (M5.BtnB.isPressed()) {
    if (g_btnBAt == 0) { g_btnBAt = now; g_btnBHold = false; }
    else if (!g_btnBHold && now - g_btnBAt >= BTN_HOLD_MS) {
      g_btnBHold = true;
      g_userForcedAp = !g_userForcedAp;
      Serial.printf("[btn] B held: userForcedAp=%d\n", g_userForcedAp ? 1 : 0);
      if (g_userForcedAp) {
        web_config::end();
        web_config::begin(true);
      } else {
        web_config::end();
        WiFi.mode(WIFI_STA);
        g_lastWifiRetryMs = 0;
      }
    }
  } else {
    if (g_btnBAt != 0 && !g_btnBHold) {
      if (g_approval.valid) sendDecision("deny");
    }
    g_btnBAt = 0;
  }
}

// =============================================================================
// Setup / Loop
// =============================================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Speaker.end();              // 不用音频, 省电 (也避免干扰 IR)
  M5.Power.setExtOutput(false);

  Serial.begin(115200);
  delay(150);
  Serial.println("\n=== agent_approver boot ===");

  app_prefs::begin();
  g_brightIdx = 1;
  for (int i = 0; i < (int)sizeof(BRIGHT_LEVELS); ++i)
    if (BRIGHT_LEVELS[i] == app_prefs::brightness()) g_brightIdx = i;
  applyBrightness();

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H))
    Serial.println("[boot] canvas createSprite FAILED");

  // boot 提示
  canvas.fillScreen(COL_BG);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(COL_BLUE);
  canvas.drawString("Agent Approver", SCREEN_W / 2, SCREEN_H / 2 - 12);
  canvas.setTextColor(COL_DIM);
  canvas.drawString("connecting...", SCREEN_W / 2, SCREEN_H / 2 + 12);
  canvas.pushSprite(0, 0);

  bootConnectWifi();
  if (WiFi.status() == WL_CONNECTED) web_config::begin(false);
  else                              web_config::begin(true);

  Serial.printf("[boot] relay url = '%s'\n", app_prefs::relayUrl().c_str());
}

void loop() {
  M5.update();
  web_config::loop();
  uint32_t now = millis();

  handleButtons(now);
  maybeRetryWifi(now);

  if (web_config::consumeNewCredFlag()) onWebSavedNewCred();
  if (web_config::consumeRebootFlag()) { delay(300); ESP.restart(); }

  // STA 上线后若之前是 SoftAP 且用户没强制 -> 关 AP
  static wl_status_t lastSt = WL_IDLE_STATUS;
  if (WiFi.status() != lastSt) {
    bool nowUp = (WiFi.status() == WL_CONNECTED);
    lastSt = WiFi.status();
    if (nowUp) {
      enableWifiPowerSave();
      if (web_config::isSoftApOn() && !g_userForcedAp) {
        web_config::end();
        WiFi.mode(WIFI_STA);
        web_config::begin(false);
      }
    }
  }

  String relay = app_prefs::relayUrl();
  bool canPoll = (WiFi.status() == WL_CONNECTED) && relay.length() > 0 &&
                 !web_config::isSoftApOn();

  if (canPoll) {
    if (g_approval.valid) {
      // 审批中: 短轮询保活 + 检测 relay 端超时, 主循环保持灵敏接收按键
      if (now - g_lastPollMs >= 2500) doPoll(relay, 0, 6000);
    } else {
      // 空闲: 先画一帧再 long-poll (阻塞期间屏幕是新的)
      renderFrame();
      doPoll(relay, 6, 12000);
    }
  }

  renderFrame();

  static uint32_t lastReport = 0;
  if (now - lastReport >= 10000) {
    lastReport = now;
    Serial.printf("[%lu] wifi=%d ap=%d ver=%d appr=%d heap=%u\n",
                  (unsigned long)now, (int)WiFi.status(),
                  web_config::isSoftApOn() ? 1 : 0, g_version,
                  g_approval.valid ? 1 : 0, ESP.getFreeHeap());
  }

  delay(g_approval.valid ? 20 : 40);
}
