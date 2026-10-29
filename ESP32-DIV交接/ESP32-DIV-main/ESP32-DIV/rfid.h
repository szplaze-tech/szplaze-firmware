#pragma once

#include <stdint.h>

/** PN532-based RFID/NFC (ported from NullTag-style flows). Install "Adafruit PN532". */
namespace RfidNfc {

/** 把 PN532 硬复位一次（拉低 RSTPDN 再释放，等芯片启动）。
 *  进功能前**必须先调**：RSTPDN 悬空的模块会停在复位态、连 I2C 地址 0x24
 *  都不应答 —— 不先复位就直接探测，必然报 "not found"。
 *  未接 RSTPDN（PN532_I2C_RST < 0）或 SPI 模式下是空操作。 */
void hardwareReset();

/** 探测 PN532。
 *  I2C 模式（V2 默认，见 shared.h 的 RFID_USE_I2C）：地址固定 0x24，
 *  走板上已有的 I2C 总线（GPIO8/9），不占用 SPI，SD 卡保持挂载。
 *  SPI 模式：软件 SPI（bitbang），失败会重挂 SD。 */
bool begin();

/** True after successful begin(). */
bool hardwareOk();

/** Clear cached clone source (optional). */
void resetCloneBuffer();

/** Clear session-retry flag before entering a feature loop. */
void clearSessionRetry();

/** @return true once if user pressed Ready on a failure result screen. */
bool consumeSessionRetry();

void sessionCardReader();
void sessionClone();
void sessionErase();
void sessionDump();
void sessionDecodeAccess();
void sessionJamReader();
void sessionDisruptEmulate();
void sessionTagDisrupt();

} // namespace RfidNfc
