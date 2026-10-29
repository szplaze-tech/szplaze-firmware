# ESP32-DIV RF 信号分析设备（SZ-plaze 二次开发版）

本项目为 SZ-plaze 基于 [cifertech/ESP32-DIV](https://github.com/cifertech/ESP32-DIV) 的二次开发版本，
包含完整工程源码、交接文档与固件发布。

> 原项目版权归原作者所有，本项目遵循其开源许可协议进行修改与分发。
> 修改内容与修复记录见 `ESP32-DIV交接/修复记录.txt`。

## 目录结构

```
├── ESP32-DIV交接/                  # 全部交接资料
│   ├── ESP32-DIV-main/             # 完整工程源码（Arduino）
│   ├── ESP32-DIV_V2_业务功能测试手册.md
│   ├── ESP32-DIV_工程分析报告.md
│   ├── ESP32_DIV_蓝牙功能架构.png
│   ├── 板子原理图/                 # 硬件原理图
│   ├── 测试代码以及文档/           # 各功能模块测试用例与代码
│   ├── 修复记录.txt                # 二次开发修改记录
│   └── 项目交接文档.docx
```

## 固件下载

正式固件发布在 [Releases 页面](../../releases)，普通用户无需注册 GitHub 账号即可下载。

| 版本 | 日期 | 说明 |
|---|---|---|
| 待发布 | - | - |

## 编译环境

1. 安装 [Arduino IDE](https://www.arduino.cc/en/software)
2. 安装 ESP32 支持包（Boards Manager 搜索 `esp32`，或参考乐鑫官方文档）
3. 打开 `ESP32-DIV交接/ESP32-DIV-main/ESP32-DIV/ESP32-DIV.ino`
4. 选择对应板型与端口，编译上传

> 注：原交接资料中的 `esp32.zip` 离线工具链包因体积原因未纳入仓库，
> 统一从乐鑫官方渠道获取，避免版本混乱。

## 反馈问题

- 提 [Issue](../../issues/new)，标题格式：`[Bug] 现象一句话` 或 `[Feature] 需求一句话`
- 请附：固件版本、硬件版本、复现步骤、串口日志

## License

遵循原项目开源协议；SZ-plaze 修改部分同样开源。
固件二进制仅限搭配本司硬件使用。
