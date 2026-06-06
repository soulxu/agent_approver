// ============================================================================
// agent_approver - M5StickS3 上的 "AI agent 审批 + 活动" 终端 (纯 BLE 版)
//
// 板子: M5Stack StickS3 (ESP32-S3-PICO, 8MB Flash + 8MB PSRAM)
// 屏幕: ST7789P3 135x240, rotation=1 横屏 240x135
// 按键: BtnA = 正面 M5 键, BtnB = 侧面键
//
// 干什么:
//   Mac 上跑 relay.py (蓝牙 central), Cursor/Claude/Codex 的 hook 把 "要批准的
//   操作" 和 "每个 agent 正在做什么" 推给 relay, relay 通过 BLE 直连推到 StickS3:
//     - 有待审批 -> 大屏显示命令, BtnA 批准 / BtnB 拒绝, 结果 notify 回 relay -> agent.
//     - 没待审批 -> 总览屏: 同时列出多个 agent 各自的当前状态.
//
// 按键 (无待审批时):
//   总览屏: BtnA 短按 = 选下一个 agent;  BtnB 短按 = 进入选中 agent 的详情.
//   详情屏: BtnA 短按 = 看下一个 agent;  BtnB 短按 = 返回总览.
//   BtnA 长按 = 切屏幕亮度;  BtnB 长按 = 清除 BLE 绑定 (重新配对用).
// 有待审批时: BtnA 单击 = 翻页(看长命令), BtnA 双击 = 批准, BtnB = 拒绝,
//             BtnB 长按 = 清除绑定.
//
// 连接/信任: StickS3 当 BLE 外设广播为 "AgentApprover", Mac 连过来.
//   首次连接用 BLE 配对+绑定 (加密, Just Works), 之后长期信任. 不用 WiFi/证书.
// ============================================================================

#include <M5Unified.h>

#include <vector>

#include "app_prefs.h"
#include "ble_link.h"

// ArduinoJson + BLE 回调吃栈, 多给点.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;

static const char* BLE_NAME = "AgentApprover";

static M5Canvas canvas(&M5.Display);

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

// 调色板
static const uint16_t COL_BG    = rgb565(8, 10, 16);
static const uint16_t COL_FG    = rgb565(235, 238, 245);
static const uint16_t COL_DIM   = rgb565(120, 130, 150);
static const uint16_t COL_AMBER = rgb565(255, 186, 64);
static const uint16_t COL_GREEN = rgb565(60, 200, 110);
static const uint16_t COL_RED   = rgb565(235, 70, 70);
static const uint16_t COL_BLUE  = rgb565(70, 160, 255);
static const uint16_t COL_PANEL = rgb565(20, 24, 34);

// ---------------- 状态 ----------------
static ble_link::ApprovalView g_approval;
static uint32_t g_lastVersion = 0xFFFFFFFF;
static String   g_toast;
static uint32_t g_toastUntil = 0;

// 多 agent 总览/详情
enum UiMode { UI_OVERVIEW, UI_DETAIL };
static UiMode g_ui = UI_OVERVIEW;
static int    g_sel = 0;            // 选中的 agent 序号 (0 = 最近活跃)
static constexpr int OVERVIEW_ROWS = 5;

static const uint8_t BRIGHT_LEVELS[] = {64, 128, 200};
static int g_brightIdx = 1;

// 提示音: 记住上次的计数, 变大就响
static uint32_t g_lastApprovalChime = 0;
static uint32_t g_lastDoneChime = 0;
static bool     g_buzz = false;  // 提示音风格: false=经典升调; true=低频嗡嗡

// 按键 (A/B 都支持短按/长按)
static uint32_t g_btnAAt = 0; static bool g_btnAHold = false;
static uint32_t g_btnBAt = 0; static bool g_btnBHold = false;
static constexpr uint32_t BTN_HOLD_MS = 800;

// 审批屏: A 单击翻页 / 双击批准
static int      g_apprPage = 0;        // 当前页
static int      g_apprPageCount = 1;   // 上次渲染算出的总页数

// 详情屏: A 翻页看 agent 返回的总结
static int      g_detailPage = 0;
static int      g_detailPageCount = 1;
static bool     g_aClickPending = false;
static uint32_t g_aClickAt = 0;
static constexpr uint32_t DOUBLE_MS = 350;

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
    canvas.drawString("...", x + maxW - 18, y + (maxLines - 1) * lineH);
  }
  return line;
}

// 把文本按当前字体宽度折成多行 (供审批屏分页). 调用前先 setFont.
static std::vector<String> wrapLines(const String& s, int maxW) {
  std::vector<String> out;
  String cur = "";
  size_t i = 0;
  while (i < s.length()) {
    int len = utf8Len((uint8_t)s[i]);
    if (i + len > s.length()) len = s.length() - i;
    String g = s.substring(i, i + len);
    i += len;
    if (g == "\n") { out.push_back(cur); cur = ""; continue; }
    if (g == "\r") continue;
    if (canvas.textWidth(cur + g) > maxW && cur.length() > 0) {
      out.push_back(cur);
      cur = g;
    } else {
      cur += g;
    }
  }
  if (cur.length() > 0) out.push_back(cur);
  return out;
}

static void applyBrightness() {
  M5.Display.setBrightness(BRIGHT_LEVELS[g_brightIdx]);
}

// 提示音. g_buzz=false: 经典升调 (approval 急促升调 / 完成柔和两声).
//        g_buzz=true:  低频嗡嗡 (approval 急促三声短嗡 / 完成一声长嗡).
static void playChime(bool approval) {
  if (g_buzz) {
    M5.Speaker.setVolume(255);
    if (approval) {
      for (int i = 0; i < 3; i++) {
        M5.Speaker.tone(150, 110); delay(130);
        M5.Speaker.stop();         delay(70);
      }
    } else {
      M5.Speaker.tone(130, 320); delay(340);
      M5.Speaker.stop();
    }
    return;
  }
  M5.Speaker.setVolume(180);
  if (approval) {
    M5.Speaker.tone(1175, 90);  delay(110);
    M5.Speaker.tone(1568, 150); delay(160);
  } else {
    M5.Speaker.tone(988, 110);  delay(120);
    M5.Speaker.tone(1319, 170); delay(180);
  }
}

static void checkChimes() {
  uint32_t a = ble_link::approvalChimes();
  if (a != g_lastApprovalChime) { g_lastApprovalChime = a; playChime(true); }
  uint32_t d = ble_link::doneChimes();
  if (d != g_lastDoneChime) { g_lastDoneChime = d; playChime(false); }
}

// 等待审批时闪烁内置 RGB 灯 (红); 没审批时熄灭.
static void updateLed(uint32_t now) {
  static bool on = false;
  static uint32_t lastToggle = 0;
  static int8_t lastState = -1;  // 0=灭 1=闪, 初值 -1 强制刷一次
  if (g_approval.valid) {
    if (now - lastToggle >= 350) {
      lastToggle = now;
      on = !on;
      rgbLedWrite(RGB_BUILTIN, on ? 90 : 0, 0, 0);  // 红
    }
    lastState = 1;
  } else if (lastState != 0) {
    on = false;
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
    lastState = 0;
  }
}

static void setToast(const String& t) {
  g_toast = t;
  g_toastUntil = millis() + 1200;
}

static void drawBleBadge() {
  bool on = ble_link::connected();
  canvas.setTextDatum(TR_DATUM);
  canvas.setTextColor(on ? COL_GREEN : COL_RED);
  canvas.setFont(&fonts::efontCN_16);
  canvas.drawString(on ? "BLE \u00b7" : "BLE \u00d7", SCREEN_W - 6, 3);
}

// 把字符串裁到 maxW 像素 (按字体), 超出加省略号. 调用前先 setFont.
static String clipToWidth(const String& s, int maxW) {
  if (canvas.textWidth(s) <= maxW) return s;
  String out = "";
  size_t i = 0;
  while (i < s.length()) {
    int len = utf8Len((uint8_t)s[i]);
    if (i + len > s.length()) len = s.length() - i;
    String g = s.substring(i, i + len);
    if (canvas.textWidth(out + g + "\u2026") > maxW) break;
    out += g;
    i += len;
  }
  return out + "\u2026";
}

static uint16_t stateColor(const String& st) {
  if (st == "wait") return COL_RED;
  if (st == "idle") return COL_GREEN;
  if (st == "busy") return COL_AMBER;
  return COL_DIM;
}

static const char* stateText(const String& st) {
  if (st == "wait") return "\u7b49\u5f85\u5ba1\u6279";  // 等待审批
  if (st == "idle") return "\u7a7a\u95f2";              // 空闲
  if (st == "busy") return "\u8fd0\u884c\u4e2d";        // 运行中
  return st.c_str();
}

static void drawToast() {
  if (g_toast.length() > 0 && millis() < g_toastUntil) {
    canvas.setTextDatum(BR_DATUM);
    canvas.setTextColor(COL_GREEN);
    canvas.setFont(&fonts::efontCN_16);
    canvas.drawString(g_toast, SCREEN_W - 6, SCREEN_H - 4);
  }
}

// =============================================================================
// 屏幕
// =============================================================================
static void renderDisconnected() {
  canvas.fillScreen(COL_BG);
  canvas.fillRect(0, 0, SCREEN_W, 22, COL_PANEL);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(COL_BLUE);
  canvas.drawString("Agent Approver", 8, 3);
  drawBleBadge();

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(COL_DIM);
  canvas.drawString("\u7b49\u5f85 Mac \u8fde\u63a5 (BLE)", SCREEN_W / 2, SCREEN_H / 2 - 8);  // 等待 Mac 连接 (BLE)
  canvas.setFont(&fonts::efontCN_14);
  canvas.drawString("\u5e7f\u64ad\u4e3a: " + String(BLE_NAME), SCREEN_W / 2, SCREEN_H / 2 + 16);  // 广播为:
  canvas.pushSprite(0, 0);
}

static void renderNoAgents() {
  canvas.fillScreen(COL_BG);
  canvas.fillRect(0, 0, SCREEN_W, 22, COL_PANEL);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(COL_BLUE);
  canvas.drawString("Agent Approver", 8, 3);
  drawBleBadge();

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(COL_DIM);
  canvas.drawString("\u7b49\u5f85 agent...", SCREEN_W / 2, SCREEN_H / 2);  // 等待 agent...
  drawToast();
  canvas.pushSprite(0, 0);
}

// 总览: 同时列出多个 agent, 高亮选中那行.
static void renderOverview() {
  int n = ble_link::agentCount();
  canvas.fillScreen(COL_BG);
  canvas.fillRect(0, 0, SCREEN_W, 22, COL_PANEL);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(COL_BLUE);
  canvas.drawString("Agents " + String(n), 8, 3);
  drawBleBadge();

  // 让选中项始终可见的滚动起点
  int start = 0;
  if (g_sel >= OVERVIEW_ROWS) start = g_sel - OVERVIEW_ROWS + 1;

  const int rowH = 21;
  const int top = 25;
  for (int row = 0; row < OVERVIEW_ROWS; ++row) {
    int idx = start + row;
    if (idx >= n) break;
    ble_link::AgentView a;
    if (!ble_link::agentAt(idx, a)) continue;
    int y = top + row * rowH;
    bool selected = (idx == g_sel);
    if (selected) {
      canvas.fillRoundRect(2, y - 1, SCREEN_W - 4, rowH - 1, 4, COL_PANEL);
      canvas.fillRect(2, y - 1, 3, rowH - 1, stateColor(a.state));
    }
    // 状态圆点
    canvas.fillCircle(14, y + 9, 4, stateColor(a.state));
    // 标签 (状态色)
    canvas.setFont(&fonts::efontCN_16);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(selected ? COL_FG : stateColor(a.state));
    String label = a.label.length() ? a.label : String("agent");
    int labelMaxW = 96;
    String lc = clipToWidth(label, labelMaxW);
    canvas.drawString(lc, 26, y + 2);
    int lw = canvas.textWidth(lc);
    // 当前在做什么 (dim)
    int textX = 26 + lw + 8;
    canvas.setTextColor(selected ? COL_FG : COL_DIM);
    canvas.drawString(clipToWidth(a.text, SCREEN_W - 6 - textX), textX, y + 2);
  }

  if (n > OVERVIEW_ROWS) {
    canvas.setTextDatum(BR_DATUM);
    canvas.setTextColor(COL_DIM);
    canvas.setFont(&fonts::efontCN_14);
    canvas.drawString(String(g_sel + 1) + "/" + String(n), SCREEN_W - 6, SCREEN_H - 2);
  }
  drawToast();
  canvas.pushSprite(0, 0);
}

// 详情: 单个 agent 的完整状态.
static void renderDetail() {
  int n = ble_link::agentCount();
  ble_link::AgentView a;
  if (n == 0 || !ble_link::agentAt(g_sel, a)) { renderOverview(); return; }

  canvas.fillScreen(COL_BG);
  uint16_t sc = stateColor(a.state);
  canvas.fillRect(0, 0, SCREEN_W, 22, COL_PANEL);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(sc);
  canvas.drawString(clipToWidth(a.label.length() ? a.label : String("agent"), 150), 8, 3);
  canvas.setTextDatum(TR_DATUM);
  canvas.setTextColor(COL_DIM);
  canvas.drawString(String(g_sel + 1) + "/" + String(n), SCREEN_W - 6, 3);

  // 状态行
  canvas.setTextDatum(TL_DATUM);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextColor(sc);
  canvas.fillCircle(12, 34, 4, sc);
  canvas.drawString(stateText(a.state), 24, 26);

  // 正文: 优先显示 agent 返回的总结 (完成后), 没有就显示当前在做什么. 支持翻页.
  bool hasSummary = a.summary.length() > 0;
  String body = hasSummary ? a.summary : a.text;

  const int top = 48;
  const int lineH = 18;
  const int hintY = SCREEN_H - 18;
  int linesPerPage = (hintY - top) / lineH;
  if (linesPerPage < 1) linesPerPage = 1;

  canvas.setFont(&fonts::efontCN_16);
  std::vector<String> lines = wrapLines(body, SCREEN_W - 12);
  int total = (int)lines.size();
  int pages = (total + linesPerPage - 1) / linesPerPage;
  if (pages < 1) pages = 1;
  g_detailPageCount = pages;
  if (g_detailPage >= pages) g_detailPage = pages - 1;
  if (g_detailPage < 0) g_detailPage = 0;

  int startLine = g_detailPage * linesPerPage;
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(hasSummary ? COL_FG : COL_DIM);
  for (int r = 0; r < linesPerPage; ++r) {
    int li = startLine + r;
    if (li >= total) break;
    canvas.drawString(lines[li], 6, top + r * lineH);
  }

  // 底部提示
  canvas.setFont(&fonts::efontCN_14);
  canvas.setTextDatum(BL_DATUM);
  if (pages > 1) {
    canvas.setTextColor(COL_BLUE);
    canvas.drawString("A \u7ffb\u9875 " + String(g_detailPage + 1) + "/" + String(pages), 6, SCREEN_H - 3);  // A 翻页 x/n
  } else {
    canvas.setTextColor(COL_DIM);
    canvas.drawString(hasSummary ? "\u603b\u7ed3" : "\u72b6\u6001", 6, SCREEN_H - 3);  // 总结 / 状态
  }
  canvas.setTextDatum(BR_DATUM);
  canvas.setTextColor(COL_DIM);
  canvas.drawString("B \u8fd4\u56de", SCREEN_W - 6, SCREEN_H - 3);  // B 返回
  drawToast();
  canvas.pushSprite(0, 0);
}

static void renderApproval() {
  canvas.fillScreen(COL_BG);
  const ble_link::ApprovalView& a = g_approval;

  canvas.fillRect(0, 0, SCREEN_W, 20, rgb565(60, 40, 0));
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(COL_AMBER);
  canvas.drawString(clipToWidth("[" + a.agent + "] " + a.tool, SCREEN_W - 60), 6, 2);
  if (a.count > 1) {
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString("x" + String(a.count), SCREEN_W - 6, 2);
  }

  // 倒计时条
  if (a.timeoutMs > 0) {
    uint32_t age = millis() - a.recvMs;
    float frac = 1.0f - (float)age / (float)a.timeoutMs;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    canvas.fillRect(0, 20, (int)(SCREEN_W * frac), 3, COL_AMBER);
  }

  // 命令标题: 固定显示在顶部 (最多 2 行), 不参与翻页, 任何页都能看到.
  const int titleY = 26;
  const int titleLineH = 18;
  canvas.setFont(&fonts::efontCN_16);
  std::vector<String> tlines = wrapLines(a.title, SCREEN_W - 12);
  int titleRows = (int)tlines.size();
  if (titleRows > 2) titleRows = 2;
  if (titleRows < 1) titleRows = 1;
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(COL_FG);
  for (int r = 0; r < titleRows; ++r) {
    String ln = tlines[r];
    if (r == titleRows - 1 && (int)tlines.size() > titleRows) ln = clipToWidth(ln + "\u2026", SCREEN_W - 12);
    canvas.drawString(ln, 6, titleY + r * titleLineH);
  }
  int titleBottom = titleY + titleRows * titleLineH;
  canvas.drawFastHLine(6, titleBottom + 1, SCREEN_W - 12, rgb565(50, 55, 70));

  // 可翻页区: 命令的完整 detail (参数/路径等)
  const int detTop = titleBottom + 5;
  const int lineH = 16;
  const int hintY = SCREEN_H - 18;
  int linesPerPage = (hintY - detTop) / lineH;
  if (linesPerPage < 1) linesPerPage = 1;

  bool hasDetail = a.detail.length() > 0 && a.detail != a.title;
  canvas.setFont(&fonts::efontCN_14);
  std::vector<String> lines = hasDetail ? wrapLines(a.detail, SCREEN_W - 12)
                                        : std::vector<String>();
  int total = (int)lines.size();
  int pages = (total + linesPerPage - 1) / linesPerPage;
  if (pages < 1) pages = 1;
  g_apprPageCount = pages;
  if (g_apprPage >= pages) g_apprPage = pages - 1;
  if (g_apprPage < 0) g_apprPage = 0;

  int startLine = g_apprPage * linesPerPage;
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(COL_DIM);
  for (int r = 0; r < linesPerPage; ++r) {
    int li = startLine + r;
    if (li >= total) break;
    canvas.drawString(lines[li], 6, detTop + r * lineH);
  }

  // 底部提示行: 翻页指示 + 操作
  canvas.setFont(&fonts::efontCN_14);
  canvas.setTextDatum(BL_DATUM);
  if (pages > 1) {
    canvas.setTextColor(COL_BLUE);
    canvas.drawString("A \u7ffb\u9875 " + String(g_apprPage + 1) + "/" + String(pages), 6, SCREEN_H - 3);  // A 翻页 x/n
  }
  canvas.setTextDatum(BR_DATUM);
  canvas.setTextColor(COL_GREEN);
  canvas.drawString("A\u00d72 \u6279\u51c6", SCREEN_W - 70, SCREEN_H - 3);   // A×2 批准
  canvas.setTextColor(COL_RED);
  canvas.drawString("B \u62d2\u7edd", SCREEN_W - 6, SCREEN_H - 3);            // B 拒绝
  canvas.pushSprite(0, 0);
}

static void renderFrame() {
  if (!ble_link::connected()) {
    renderDisconnected();
  } else if (g_approval.valid) {
    renderApproval();
  } else if (ble_link::agentCount() == 0) {
    renderNoAgents();
  } else if (g_ui == UI_DETAIL) {
    renderDetail();
  } else {
    renderOverview();
  }
}

// =============================================================================
// 从 ble_link 同步快照到本地 (主循环用), 新审批到来时拉满亮度
// =============================================================================
static void syncState() {
  bool wasValid = g_approval.valid;
  String prevId = g_approval.id;

  if (!ble_link::currentApproval(g_approval)) g_approval.valid = false;

  // 保证选中序号在范围内
  int n = ble_link::agentCount();
  if (n == 0) { g_sel = 0; }
  else if (g_sel >= n) { g_sel = n - 1; }
  if (g_sel < 0) g_sel = 0;

  // 换了一条审批 -> 翻页归零, 清掉待定的单/双击
  if (g_approval.valid && (!wasValid || g_approval.id != prevId)) {
    g_apprPage = 0;
    g_aClickPending = false;
    M5.Display.setBrightness(255);
  } else if (!g_approval.valid && wasValid) {
    g_aClickPending = false;
    applyBrightness();
  }
}

// =============================================================================
// 按键
// =============================================================================
static void cycleBrightness() {
  g_brightIdx = (g_brightIdx + 1) % (int)sizeof(BRIGHT_LEVELS);
  applyBrightness();
  app_prefs::setBrightness(BRIGHT_LEVELS[g_brightIdx]);
}

static void selectNext() {
  int n = ble_link::agentCount();
  if (n <= 0) return;
  g_sel = (g_sel + 1) % n;
  g_detailPage = 0;
}

static void detailNextPage() {
  if (g_detailPageCount <= 1) return;
  g_detailPage = (g_detailPage + 1) % g_detailPageCount;
}

static void doApprove() {
  bool ok = ble_link::decideFirst("allow");
  setToast(ok ? "\u5df2\u6279\u51c6" : "\u672a\u8fde\u63a5");  // 已批准 / 未连接
  if (ok) { g_ui = UI_OVERVIEW; syncState(); applyBrightness(); }  // 处理完回列表
}

static void approvalNextPage() {
  if (g_apprPageCount <= 1) return;
  g_apprPage = (g_apprPage + 1) % g_apprPageCount;
}

// 在 经典提示音 / 嗡嗡提示音 之间切换, 并试听一下当前风格.
static void toggleChimeStyle() {
  g_buzz = !g_buzz;
  app_prefs::setSoundBuzz(g_buzz);
  setToast(g_buzz ? "\u63d0\u793a\u97f3: \u55e1\u55e1" : "\u63d0\u793a\u97f3: \u7ecf\u5178");  // 提示音: 嗡嗡 / 经典
  playChime(false);  // 试听
}

// BtnA 短按: 审批屏 -> 单击翻页 / 双击批准; 列表/详情屏 -> 单击切换 / 双击换提示音
static void onAShort() {
  uint32_t now = millis();
  if (g_approval.valid) {
    if (g_aClickPending && (now - g_aClickAt) <= DOUBLE_MS) {
      g_aClickPending = false;   // 双击 -> 批准
      doApprove();
    } else {
      g_aClickPending = true;    // 先挂起, 等一个双击窗口看是不是翻页
      g_aClickAt = now;
    }
  } else if (ble_link::connected() && ble_link::agentCount() > 0) {
    if (g_aClickPending && (now - g_aClickAt) <= DOUBLE_MS) {
      g_aClickPending = false;   // 双击 -> 切换提示音风格
      toggleChimeStyle();
    } else {
      g_aClickPending = true;    // 先挂起, 等一个双击窗口看是不是单击翻页/选下一个
      g_aClickAt = now;
    }
  } else {
    cycleBrightness();
  }
}

// 单击挂起超过双击窗口 -> 确认是单击 -> 执行对应单击动作
static void resolvePendingClick(uint32_t now) {
  if (!g_aClickPending) return;
  if (now - g_aClickAt <= DOUBLE_MS) return;
  g_aClickPending = false;
  if (g_approval.valid) {
    approvalNextPage();                       // 审批屏: 翻页
  } else if (ble_link::connected() && ble_link::agentCount() > 0) {
    if (g_ui == UI_DETAIL) detailNextPage();  // 详情屏: 翻页看总结
    else selectNext();                        // 总览屏: 选下一个
  }
}

// BtnA 长按 -> 亮度
static void onAHold() { cycleBrightness(); }

// BtnB 短按
static void onBShort() {
  if (g_approval.valid) {
    bool ok = ble_link::decideFirst("deny");
    setToast(ok ? "\u5df2\u62d2\u7edd" : "\u672a\u8fde\u63a5");  // 已拒绝 / 未连接
    if (ok) { g_ui = UI_OVERVIEW; syncState(); applyBrightness(); }  // 处理完回列表
  } else if (ble_link::connected() && ble_link::agentCount() > 0) {
    g_ui = (g_ui == UI_OVERVIEW) ? UI_DETAIL : UI_OVERVIEW;
    g_detailPage = 0;  // 进入详情从第一页开始
  }
}

// BtnB 长按 -> 清绑定
static void onBHold() {
  ble_link::clearBonds();
  setToast("\u5df2\u6e05\u9664\u7ed1\u5b9a");  // 已清除绑定
}

static void handleButtons(uint32_t now) {
  // BtnA: 短按/长按
  if (M5.BtnA.isPressed()) {
    if (g_btnAAt == 0) { g_btnAAt = now; g_btnAHold = false; }
    else if (!g_btnAHold && now - g_btnAAt >= BTN_HOLD_MS) {
      g_btnAHold = true;
      onAHold();
    }
  } else {
    if (g_btnAAt != 0 && !g_btnAHold) onAShort();
    g_btnAAt = 0;
  }

  // BtnB: 短按/长按
  if (M5.BtnB.isPressed()) {
    if (g_btnBAt == 0) { g_btnBAt = now; g_btnBHold = false; }
    else if (!g_btnBHold && now - g_btnBAt >= BTN_HOLD_MS) {
      g_btnBHold = true;
      onBHold();
    }
  } else {
    if (g_btnBAt != 0 && !g_btnBHold) onBShort();
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
  M5.Speaker.begin();
  M5.Speaker.setVolume(180);
  M5.Power.setExtOutput(false);

  Serial.begin(115200);
  delay(150);
  Serial.println("\n=== agent_approver (BLE) boot ===");

  app_prefs::begin();
  g_brightIdx = 1;
  for (int i = 0; i < (int)sizeof(BRIGHT_LEVELS); ++i)
    if (BRIGHT_LEVELS[i] == app_prefs::brightness()) g_brightIdx = i;
  applyBrightness();
  g_buzz = app_prefs::soundBuzz();

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H))
    Serial.println("[boot] canvas createSprite FAILED");

  canvas.fillScreen(COL_BG);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(COL_BLUE);
  canvas.drawString("Agent Approver", SCREEN_W / 2, SCREEN_H / 2 - 12);
  canvas.setTextColor(COL_DIM);
  canvas.drawString("BLE \u542f\u52a8\u4e2d...", SCREEN_W / 2, SCREEN_H / 2 + 12);  // BLE 启动中...
  canvas.pushSprite(0, 0);

  ble_link::begin(BLE_NAME);
}

void loop() {
  M5.update();
  uint32_t now = millis();

  handleButtons(now);
  resolvePendingClick(now);

  uint32_t v = ble_link::version();
  if (v != g_lastVersion) {
    g_lastVersion = v;
    syncState();
  }

  checkChimes();
  updateLed(now);

  renderFrame();

  static uint32_t lastReport = 0;
  if (now - lastReport >= 10000) {
    lastReport = now;
    Serial.printf("[%lu] ble=%d appr=%d heap=%u\n", (unsigned long)now,
                  ble_link::connected() ? 1 : 0, g_approval.valid ? 1 : 0,
                  ESP.getFreeHeap());
  }

  delay(g_approval.valid ? 30 : 60);
}
