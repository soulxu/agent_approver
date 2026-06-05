// =============================================================================
// wifi_store.h - 用 NVS (Preferences) 持久化 WiFi 凭据 (跟 stick_s3_eyes 同一套).
//
// 数据布局: namespace = "aawifi", 单个 key "json" = JSON 字符串
//   {"creds":[{"ssid":"home","password":"xxx"}, ...]}
// 最多 MAX_CREDS 条, 新加的放最前 (最高优先级), 重名替换.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <vector>

namespace wifi_store {

struct Cred {
  String ssid;
  String password;
};

constexpr size_t MAX_CREDS = 8;

std::vector<Cred> loadAll();
bool addOrUpdate(const String& ssid, const String& password);
bool remove(const String& ssid);
bool clearAll();

}  // namespace wifi_store
