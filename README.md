# szplaze-firmware

本仓库仅发布固件二进制文件（bin），不包含源码。

## ESP32-DIV 固件 v0.2.0

| 文件 | 说明 |
|---|---|
| `release/ESP32-DIV.ino.bin` | 应用固件 |
| `release/ESP32-DIV.ino.bootloader.bin` | Bootloader |
| `release/ESP32-DIV.ino.partitions.bin` | 分区表 |
| `release/ESP32-DIV.ino.SHA256SUMS.txt` | SHA256 校验（见 `release/SHA256SUMS.txt`） |

## 烧录（ESP32，esptool）

```
esptool.py --chip esp32 --port COMx --baud 921600 \
  write_flash 0x1000 ESP32-DIV.ino.bootloader.bin \
              0x8000 ESP32-DIV.ino.partitions.bin \
              0x10000 ESP32-DIV.ino.bin
```

校验：`sha256sum -c SHA256SUMS.txt`（在 `release/` 目录内执行；`.elf` 条目会提示缺失，属正常）。

最新版本请前往 [Releases](https://github.com/szplaze-tech/szplaze-firmware/releases) 页面下载。
