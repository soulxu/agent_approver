// =============================================================================
// net.h - StickS3 <-> Mac relay 的 HTTP 客户端 (明文 HTTP, LAN 内, 无 TLS).
//
//   poll():   GET  <relay>/stick/poll?v=<ver>&wait=<sec>   long-poll 拉待办
//   decide(): POST <relay>/stick/decide  {id, decision}    回传按键结果
// =============================================================================
#pragma once

#include <Arduino.h>

namespace net {

struct Approval {
  bool     valid = false;
  String   id;
  String   agent;
  String   tool;
  String   title;
  String   detail;
  String   cwd;
  int      count   = 0;
  uint32_t ageMs   = 0;
  uint32_t timeoutMs = 0;
};

struct Activity {
  bool   valid = false;
  String agent;
  String kind;
  String text;
};

struct PollResult {
  bool     ok = false;     // 这次 HTTP 拉取是否成功
  int      version = 0;
  Approval approval;
  Activity activity;
};

// waitSec: 服务端 long-poll 阻塞秒数 (0 = 立即返回). timeoutMs 必须 > waitSec*1000.
PollResult poll(const String& baseUrl, int knownVersion, int waitSec, uint32_t timeoutMs);

bool decide(const String& baseUrl, const String& id, const String& decision,
            uint32_t timeoutMs);

}  // namespace net
