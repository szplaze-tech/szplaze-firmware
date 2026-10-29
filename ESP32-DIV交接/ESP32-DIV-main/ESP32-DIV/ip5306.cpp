#include "ip5306.h"
#include <Arduino.h>

// 总线扫描诊断打印开关（[IP5306] scanning/scan/0x75 判定行）。生产默认关闭；
// 排查 I2C 时改 1 可重新看到上电总线扫描结果。
#ifndef IP5306_SCAN_DEBUG
#define IP5306_SCAN_DEBUG 0
#endif

// ----- IP5306 register map (from M5Stack Power IP5306 driver) -----
#define IP5306_REG_READ0 0x70  // charging status (bit3 = CHARGE_ENABLE / charging)
#define IP5306_REG_READ3 0x78  // battery level, high nibble encodes the step
#define CHARGE_ENABLE_BIT 0x08

static bool s_avail = false;

// Returns register value, or 0xFF on bus error / NACK. Updates s_avail every call
// so a transient bus failure is retried instead of latching the device as absent.
static uint8_t ip5306Read(uint8_t reg)
{
  Wire.beginTransmission(IP5306_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    s_avail = false;
    return 0xFF;
  }
  if (Wire.requestFrom((uint8_t)IP5306_ADDR, (uint8_t)1) != 1) {
    s_avail = false;
    return 0xFF;
  }
  s_avail = true;
  return Wire.read();
}

bool ip5306_isAvailable()
{
  ip5306Read(IP5306_REG_READ0);
  return s_avail;
}

int8_t ip5306_getBatteryPercent()
{
  const uint8_t d = ip5306Read(IP5306_REG_READ3);
  if (!s_avail) {
    return -1;
  }
  switch (d & 0xF0) {
    case 0x00: return 100;  // full
    case 0x80: return 75;
    case 0xC0: return 50;
    case 0xE0: return 25;
    default:   return 0;
  }
}

bool ip5306_isCharging()
{
  const uint8_t d = ip5306Read(IP5306_REG_READ0);
  if (!s_avail) {
    return false;
  }
  return (d & CHARGE_ENABLE_BIT) != 0;  // bit3 = 1 while charging/VIN present
}

bool ip5306_readRegister(uint8_t reg, uint8_t* out)
{
  const uint8_t d = ip5306Read(reg);
  if (out) {
    *out = d;
  }
  return s_avail;
}

void ip5306_scanBusOnce()
{
  static bool s_scanned = false;
  if (s_scanned) {
    return;
  }
  s_scanned = true; 

#if IP5306_SCAN_DEBUG
  Serial.println("[IP5306] scanning I2C bus 0x03-0x77 ...");
#endif
  uint8_t found[16];
  uint8_t n = 0;
  for (uint8_t addr = 0x03; addr <= 0x77; ++addr) {
    yield();
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0 && n < (uint8_t)(sizeof(found) / sizeof(found[0]))) {
      found[n++] = addr;
    }
  }

  if (n == 0) {
#if IP5306_SCAN_DEBUG
    Serial.println("[IP5306] scan: NO device responded (bus dead / no pull-ups / Wire not begun)");
#endif
    return;
  }

  bool has75 = false;
#if IP5306_SCAN_DEBUG
  Serial.printf("[IP5306] scan: %u device(s):", n);
  for (uint8_t i = 0; i < n; ++i) {
    Serial.printf(" 0x%02X", found[i]);
    if (found[i] == IP5306_ADDR) {
      has75 = true;
    }
  }
  Serial.println();
#else
  for (uint8_t i = 0; i < n; ++i) {
    if (found[i] == IP5306_ADDR) {
      has75 = true;
      break;
    }
  }
#endif

  if (has75) {
#if IP5306_SCAN_DEBUG
    Serial.println("[IP5306] 0x75 RESPONDING -> chip is the I2C version, driver will work");
#endif
    return;
  }

  // 0x75 缺席。实测规律（用户黄金线索）：只有 VIN 在 IP5306 自身唤醒时就在场
  // （USB-first 上电）它才初始化 I2C；电池/DC 先唤醒（BOOST 直接启动）则 I2C
  // 不初始化且不自愈——软件重启也无效（已实测两次重启后仍 NACK）。
  // 恢复途径：①短按 KEY（文档记载 KEY 脉冲可唤醒 I2C）；②USB-first 重新上电；
  // ③硬件改装：KEY 飞线到空闲 GPIO 由固件发脉冲。
  if (n >= 2) {
#if IP5306_SCAN_DEBUG
    Serial.println("[IP5306] 0x75 missing (bus alive) -> IP5306 I2C not initialized this power-up. "
                   "Recover: short-press KEY or power up with USB first.");
#endif
  } else {
#if IP5306_SCAN_DEBUG
    Serial.println("[IP5306] 0x75 missing and nothing else on bus -> check SDA/SCL wiring/solder");
#endif
  }
}
