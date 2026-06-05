// =============================================================================
// app_prefs.h - 轻量配置 (NVS Preferences), namespace = "aaprefs".
//   - brightness: 屏幕亮度 0..255 (默认 128)
// 纯 BLE 版本不再需要 relay URL: Mac 直接通过蓝牙连过来.
// =============================================================================
#pragma once

#include <Arduino.h>

namespace app_prefs {

void begin();

uint8_t  brightness();
void     setBrightness(uint8_t v);

}  // namespace app_prefs
