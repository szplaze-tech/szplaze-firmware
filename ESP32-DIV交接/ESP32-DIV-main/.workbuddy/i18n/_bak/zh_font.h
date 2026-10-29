// 自动生成（gen_all.py）—— 中文点阵字库，勿手工编辑
// 同一字符集两套字号：12x12（功能内文字）/ 16x16（菜单文字）
#pragma once
#include <Arduino.h>

#define ZH12_W      12
#define ZH12_H      12
#define ZH12_BYTES  18
#define ZH16_W      16
#define ZH16_H      16
#define ZH16_BYTES  32
#define ZH_FONT_COUNT 431

// 升序 Unicode 索引（两套字号共用同一顺序）
extern const uint16_t zhUnicode[ZH_FONT_COUNT];
extern const uint8_t  zhFont12[ZH_FONT_COUNT][ZH12_BYTES];
extern const uint8_t  zhFont16[ZH_FONT_COUNT][ZH16_BYTES];
