// =============================================================================
// ble_link.h - StickS3 当 BLE 外设 (peripheral), 跟 Mac 上的 relay 直连.
//
// 用 Nordic UART Service (NUS) 当一根 "蓝牙串口": Mac 往 RX 特征写, stick 用 TX
// 特征 notify 回去, 两个方向都跑换行分隔的 JSON. 不用 WiFi / 不用证书, 信任靠
// BLE 配对+绑定 (加密, 首次连接配对一次即信任).
//
// 收到 (Mac -> stick):
//   {"t":"approval","id","agent","tool","title","detail","cwd","timeout_ms"}
//   {"t":"cancel","id"}                  某条审批被决定/超时, 撤掉
//   {"t":"agent","id","label","state","text"}   某个 agent 的当前状态 (upsert)
//   {"t":"agent_summary","id","summary"} 某个 agent 返回的总结 (完成时详情页翻页看)
//   {"t":"agent_del","id"}               某个 agent 结束, 移除
//   {"t":"reset"}                        清空所有 agent + 待审批 (Mac 重连时发)
// 发出 (stick -> Mac):
//   {"t":"decide","id","decision":"allow"|"deny"}
// =============================================================================
#pragma once

#include <Arduino.h>

namespace ble_link {

struct ApprovalView {
  bool     valid = false;
  String   id;
  String   agent;
  String   tool;
  String   title;
  String   detail;
  String   cwd;
  int      count = 0;       // 当前待审批总数
  uint32_t timeoutMs = 0;
  uint32_t recvMs = 0;      // 收到时的 millis(), 用来画倒计时
};

// 一个 agent (一个 Cursor 对话) 的当前状态.
struct AgentView {
  bool     valid = false;
  String   id;
  String   label;          // 显示名 (一般是工程目录名)
  String   state;          // "busy" | "idle" | "wait"
  String   text;           // 当前在干什么
  String   summary;        // agent 最近一次返回的总结 (可能很长, 详情页翻页看)
  uint32_t ts = 0;         // 最近更新时的 millis()
};

void begin(const char* deviceName);

bool connected();

// 状态版本号, 每次有变化 (审批/agent/连接) +1, 给主循环判断要不要重画.
uint32_t version();

// 取当前要显示的第一条审批 (最旧的) + 总数. 没有返回 false.
bool currentApproval(ApprovalView& out);

// 多 agent 总览: 数量 + 按 "最近活跃" 排序取第 index 个 (0 = 最近).
int  agentCount();
bool agentAt(int index, AgentView& out);

// 提示音计数器 (只增不减): 主循环对比上次读到的值, 变大就响一声.
//   approvalChimes : 每来一条新的待审批 +1
//   doneChimes     : 每有一个 agent 从 "忙" 变成 "空闲/完成" +1
uint32_t approvalChimes();
uint32_t doneChimes();

// A/B 按下: 对第一条审批给出决定, notify 给 Mac, 并本地移除该条. 成功(已连接)返回 true.
bool decideFirst(const char* decision);

// 长按 B: 清掉所有 BLE 绑定信息 (重新配对用).
void clearBonds();

}  // namespace ble_link
