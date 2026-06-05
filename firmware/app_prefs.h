// =============================================================================
// app_prefs.h - 轻量配置 (NVS Preferences), namespace = "aaprefs".
//   - relayUrl:   Mac 上 relay 的地址, 形如 "http://192.168.1.100:8799" (无尾斜杠)
//   - brightness: 屏幕亮度 0..255 (默认 128)
// =============================================================================
#pragma once

#include <Arduino.h>

namespace app_prefs {

void begin();

String   relayUrl();
void     setRelayUrl(const String& url);

uint8_t  brightness();
void     setBrightness(uint8_t v);

}  // namespace app_prefs
