#pragma once

#include <Arduino.h>
#include "shared.h"
#include "i18n.h"       // LANG_EN / LANG_ZH

#ifndef SETTINGS_PATH
#define SETTINGS_PATH "/config/settings.json"
#endif

struct AppSettings {

  uint8_t  brightness = BKL_LEVEL_MED;
  Theme    theme      = Theme::Dark;
  uint8_t  accentColor = 0;

  // 按键音（蜂鸣器随按键短鸣）。2026-10-09 蜂鸣器飞线至 IO45 后启用；
  // 默认开，Settings 里可关。
  bool     keyBeepEnabled  = true;

  bool     autoWifiScan    = true;
  bool     autoBleScan     = true;

  uint8_t  lang            = LANG_EN;   // 界面语言（LANG_EN / LANG_ZH）

  uint16_t touchXMin = TOUCH_X_MIN;
  uint16_t touchXMax = TOUCH_X_MAX;
  uint16_t touchYMin = TOUCH_Y_MIN;
  uint16_t touchYMax = TOUCH_Y_MAX;

};

struct AccentOption {
  const char* name;
  uint16_t color565;
};

constexpr uint8_t ACCENT_PRESET_COUNT = 7;

AppSettings& settings();
uint8_t accentPresetClamp(uint8_t preset);
uint16_t accentColor565(uint8_t preset);
const char* accentPresetName(uint8_t preset);

const char* settingsBoardProfileId();
void settingsApplyBoardTouchDefaults();
bool settingsLoad();
bool settingsSave();
