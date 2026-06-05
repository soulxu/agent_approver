// =============================================================================
// web_config.h - SoftAP + 网页配网 (agent_approver 版)
//
// 路由:
//   GET  /         首页: 状态 + WiFi 列表 + 添加表单 + relay URL + 重启
//   POST /add      新增/更新一条 WiFi (form: ssid, password)
//   POST /delete   删除一条 WiFi (form: ssid)
//   GET  /scan     启动一次 WiFi 扫描, 重定向回 /
//   POST /relay    设置 relay URL
//   POST /reboot   重启设备
//
// 全程不阻塞主循环.
// =============================================================================
#pragma once

#include <Arduino.h>

namespace web_config {

// withSoftAp = true: 开 SoftAP "Approver-XXXX". false: 假定已 STA 连上, 只跑 HTTP.
void begin(bool withSoftAp);
void loop();
void end();

bool isRunning();
bool isSoftApOn();
String softApSsid();

bool consumeNewCredFlag();
bool consumeRebootFlag();

}  // namespace web_config
