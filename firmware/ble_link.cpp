#include "ble_link.h"

#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include <vector>

// M5StickS3 (ESP32-S3) 的 BLE 栈是 NimBLE. 清绑定用 NimBLE 的 ble_store_clear().
extern "C" int ble_store_clear(void);

namespace ble_link {

namespace {

// Nordic UART Service (NUS) UUID
constexpr const char* SVC_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* RX_UUID  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // write  (Mac->stick)
constexpr const char* TX_UUID  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // notify (stick->Mac)

constexpr size_t MAX_APPROVALS = 8;
constexpr size_t MAX_AGENTS = 12;
constexpr size_t MAX_RXBUF = 4096;

struct Approval {
  String   id, agent, tool, title, detail, cwd;
  uint32_t timeoutMs = 0;
  uint32_t recvMs = 0;
};

struct Agent {
  String   id, label, state, text, summary;
  uint32_t ts = 0;
};

BLECharacteristic* g_tx = nullptr;
BLEServer*         g_server = nullptr;

SemaphoreHandle_t       g_mtx = nullptr;
std::vector<Approval>   g_approvals;       // 受 g_mtx 保护
std::vector<Agent>      g_agents;          // 受 g_mtx 保护, 越靠前越新
volatile bool           g_connected = false;
volatile uint32_t       g_version = 0;
volatile uint32_t       g_chimeApproval = 0;  // 新待审批 +1
volatile uint32_t       g_chimeDone = 0;      // 某 agent 由忙转空闲 +1
String                  g_rxbuf;           // 只在 BLE 回调线程里用, 不跨线程

struct Lock {
  Lock()  { if (g_mtx) xSemaphoreTake(g_mtx, portMAX_DELAY); }
  ~Lock() { if (g_mtx) xSemaphoreGive(g_mtx); }
};

void bump() { g_version++; }

void notifyTx(const String& line) {
  if (!g_tx || !g_connected) return;
  String s = line;
  if (!s.endsWith("\n")) s += "\n";
  g_tx->setValue((uint8_t*)s.c_str(), s.length());
  g_tx->notify();
}

// ---- 处理一条来自 Mac 的 JSON 消息 ----
void handleMessage(const String& line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  const char* t = doc["t"] | "";

  if (strcmp(t, "approval") == 0) {
    Approval a;
    a.id        = (const char*)(doc["id"]     | "");
    a.agent     = (const char*)(doc["agent"]  | "");
    a.tool      = (const char*)(doc["tool"]   | "");
    a.title     = (const char*)(doc["title"]  | "");
    a.detail    = (const char*)(doc["detail"] | "");
    a.cwd       = (const char*)(doc["cwd"]    | "");
    a.timeoutMs = doc["timeout_ms"] | 0;
    a.recvMs    = millis();
    if (a.id.length() == 0) return;
    Lock lk;
    bool replaced = false;
    for (auto& e : g_approvals) {
      if (e.id == a.id) { e = a; replaced = true; break; }
    }
    if (!replaced && g_approvals.size() < MAX_APPROVALS) {
      g_approvals.push_back(a);
      g_chimeApproval++;   // 新待审批 -> 响铃
    }
    bump();
  } else if (strcmp(t, "cancel") == 0) {
    String id = (const char*)(doc["id"] | "");
    Lock lk;
    for (size_t i = 0; i < g_approvals.size(); ++i) {
      if (g_approvals[i].id == id) { g_approvals.erase(g_approvals.begin() + i); break; }
    }
    bump();
  } else if (strcmp(t, "agent") == 0) {
    String id = (const char*)(doc["id"] | "");
    if (id.length() == 0) return;
    Agent a;
    a.id    = id;
    a.label = (const char*)(doc["label"] | "");
    a.state = (const char*)(doc["state"] | "busy");
    a.text  = (const char*)(doc["text"]  | "");
    a.ts    = millis();
    Lock lk;
    // upsert: 先删旧的, 再插到队首 (最新). 保留已有的 summary.
    bool hadPrev = false;
    String prevState;
    for (size_t i = 0; i < g_agents.size(); ++i) {
      if (g_agents[i].id == id) {
        hadPrev = true;
        prevState = g_agents[i].state;
        a.summary = g_agents[i].summary;  // 普通状态更新不带 summary, 别丢
        g_agents.erase(g_agents.begin() + i);
        break;
      }
    }
    // 由 "忙/等待" 变成 "空闲" = 完成了一轮 -> 响铃
    if (hadPrev && prevState != "idle" && a.state == "idle") g_chimeDone++;
    g_agents.insert(g_agents.begin(), a);
    if (g_agents.size() > MAX_AGENTS) g_agents.pop_back();
    bump();
  } else if (strcmp(t, "agent_summary") == 0) {
    String id = (const char*)(doc["id"] | "");
    String sm = (const char*)(doc["summary"] | "");
    if (id.length() == 0) return;
    Lock lk;
    for (size_t i = 0; i < g_agents.size(); ++i) {
      if (g_agents[i].id == id) { g_agents[i].summary = sm; break; }
    }
    bump();
  } else if (strcmp(t, "agent_del") == 0) {
    String id = (const char*)(doc["id"] | "");
    Lock lk;
    for (size_t i = 0; i < g_agents.size(); ++i) {
      if (g_agents[i].id == id) { g_agents.erase(g_agents.begin() + i); break; }
    }
    bump();
  } else if (strcmp(t, "reset") == 0) {
    Lock lk;
    g_approvals.clear();
    g_agents.clear();
    bump();
  }
}

void feedRx(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    char c = (char)data[i];
    if (c == '\n') {
      String line = g_rxbuf;
      g_rxbuf = "";
      line.trim();
      if (line.length() > 0) handleMessage(line);
    } else if (c != '\r') {
      if (g_rxbuf.length() < MAX_RXBUF) g_rxbuf += c;
      else g_rxbuf = "";  // 行太长, 丢弃保命
    }
  }
}

// ---- 回调 ----
class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    g_connected = true;
    bump();
    Serial.println("[ble] connected");
  }
  void onDisconnect(BLEServer* s) override {
    g_connected = false;
    bump();
    Serial.println("[ble] disconnected, re-advertising");
    s->startAdvertising();
  }
};

class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    feedRx(c->getData(), c->getLength());
  }
};

class SecurityCb : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
  bool onSecurityRequest() override { return true; }
};

}  // namespace

void begin(const char* deviceName) {
  g_mtx = xSemaphoreCreateMutex();

  BLEDevice::init(deviceName);
  BLEDevice::setMTU(247);
  BLEDevice::setPower(ESP_PWR_LVL_P9);

  // 加密 + 绑定 (Just Works, 无需输码): 首次连接配一次对就长期信任.
  // 加密由下面的 BLESecurity (SC_BOND) + 特征的 ENCRYPTED 权限触发.
  BLEDevice::setSecurityCallbacks(new SecurityCb());

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCb());

  BLEService* svc = g_server->createService(SVC_UUID);

  // *_ENC 属性: 读/写这两个特征都要求加密链路 -> 连接时触发配对+绑定.
  g_tx = svc->createCharacteristic(
      TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ_ENC);
  g_tx->addDescriptor(new BLE2902());

  BLECharacteristic* rx = svc->createCharacteristic(
      RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR |
          BLECharacteristic::PROPERTY_WRITE_ENC);
  rx->setCallbacks(new RxCb());

  svc->start();

  // 加密 + 绑定 (Just Works, 无需输码).
  BLESecurity* sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);
  sec->setInitEncryptionKey();
  sec->setRespEncryptionKey();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.printf("[ble] advertising as '%s'\n", deviceName);
}

bool connected() { return g_connected; }

uint32_t version() { return g_version; }

bool currentApproval(ApprovalView& out) {
  Lock lk;
  if (g_approvals.empty()) { out.valid = false; return false; }
  const Approval& a = g_approvals.front();
  out.valid     = true;
  out.id        = a.id;
  out.agent     = a.agent;
  out.tool      = a.tool;
  out.title     = a.title;
  out.detail    = a.detail;
  out.cwd       = a.cwd;
  out.count     = (int)g_approvals.size();
  out.timeoutMs = a.timeoutMs;
  out.recvMs    = a.recvMs;
  return true;
}

int agentCount() {
  Lock lk;
  return (int)g_agents.size();
}

bool agentAt(int index, AgentView& out) {
  Lock lk;
  if (index < 0 || index >= (int)g_agents.size()) { out.valid = false; return false; }
  const Agent& a = g_agents[index];
  out.valid   = true;
  out.id      = a.id;
  out.label   = a.label;
  out.state   = a.state;
  out.text    = a.text;
  out.summary = a.summary;
  out.ts      = a.ts;
  return true;
}

uint32_t approvalChimes() { return g_chimeApproval; }
uint32_t doneChimes() { return g_chimeDone; }

bool decideFirst(const char* decision) {
  String id;
  {
    Lock lk;
    if (g_approvals.empty()) return false;
    id = g_approvals.front().id;
  }
  if (!g_connected) return false;

  JsonDocument doc;
  doc["t"] = "decide";
  doc["id"] = id;
  doc["decision"] = decision;
  String out;
  serializeJson(doc, out);
  notifyTx(out);

  {
    Lock lk;
    if (!g_approvals.empty() && g_approvals.front().id == id)
      g_approvals.erase(g_approvals.begin());
    bump();
  }
  Serial.printf("[ble] decide %s -> %s\n", id.c_str(), decision);
  return true;
}

void clearBonds() {
  int rc = ble_store_clear();
  Serial.printf("[ble] ble_store_clear rc=%d\n", rc);
}

}  // namespace ble_link
