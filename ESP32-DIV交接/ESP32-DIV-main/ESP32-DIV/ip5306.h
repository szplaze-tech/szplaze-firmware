#pragma once
#include <stdint.h>
#include <Wire.h>

// Minimal IP5306 (ESP32-DIV V2 power/charge IC) I2C helper.
// IP5306 sits on the same I2C bus as the PCF8574 keypad (SDA=GPIO8, SCL=GPIO9,
// slave address 0x75). Register/bit mapping follows the well-tested M5Stack
// Power(IP5306) driver.
//
// Charging status: REG_READ0 (0x70) bit3 = 1 -> VIN present & charging.
// Battery level : REG_READ3 (0x78) high nibble -> 0x00/0x80/0xC0/0xE0 = 100/75/50/25%.

#define IP5306_ADDR 0x75

int8_t  ip5306_getBatteryPercent(); // 0/25/50/75/100, -1 = unknown
bool    ip5306_isCharging();         // true while VIN present & charging
bool    ip5306_isAvailable();        // true if the last I2C read succeeded
// Raw register read for diagnostics. Returns true on I2C success, value in *out.
bool    ip5306_readRegister(uint8_t reg, uint8_t* out);
// One-shot full-bus scan (0x03-0x77) printed over Serial. Runs only on first call.
// Definitively tells whether 0x75 is alive on the bus (I2C-version chip) or not
// (LED-version chip / bus fault).
void    ip5306_scanBusOnce();
