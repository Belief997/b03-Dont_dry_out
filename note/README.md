# nRF52810 sensor_beacon 开发笔记

本目录汇总本项目的开发资料、编译配置、SDK 框架说明与方案设计,配合 `dev/app/sensor_beacon` 工程骨架使用。

## 项目一句话

nRF52810 + S112,常驻 **System OFF 深度睡眠**;外部比较器上升沿经 **GPIO SENSE** 唤醒(唤醒即复位),唤醒后**等待 1~2s 传感器稳定 → SAADC 采集(传感器 + 电池)→ 非连接 BLE 广播上报 → 回到深度睡眠**。跨睡眠的 8bit 事件计数器保存在 **GPREGRET** 保持寄存器中。

## 文档索引

| 文件 | 内容 |
|------|------|
| [01-sdk-structure-and-resources.md](01-sdk-structure-and-resources.md) | SDK 目录结构、支持的芯片与测试板、SoftDevice 选型、nRF52810 资源与三款芯片对比 |
| [02-toolchain-and-build-flow.md](02-toolchain-and-build-flow.md) | 开发环境/工具链、编译流程、烧录流程(SoftDevice + App)、调试与日志(RTT) |
| [03-nrf52810-project-config.md](03-nrf52810-project-config.md) | nRF52810 工程配置要点:目标选择、Makefile、链接脚本内存布局、sdk_config.h、SAADC 开启、DEVELOP_IN_NRF52832 说明 |
| [04-solution-design.md](04-solution-design.md) | 方案设计与讨论:深睡策略、SENSE 翻转状态机、GPREGRET 计数器、采集时序、广播负载、功耗预算、待定项(TODO) |
| [05-broadcast-payload-protocol.md](05-broadcast-payload-protocol.md) | **广播负载对接协议**(单向、不可连接):完整广播包字节结构、字段定义与字节序、报文示例、Python/C 解析示例、去重与兼容性建议<br>⚠ 本文写的是 v0x01,而 `main.c` 已是 v0x02、`app_proto.h` 已定义 v0x03 —— **已滞后于代码** |
| [06-ble-command-protocol.md](06-ble-command-protocol.md) | **连接态命令协议 cmd proto v1**(双向、NUS 透传):帧格式、12 条命令详表、状态码、标定模式与看门狗、记录下载与重置、标定作业流程、疏漏清单与待决项<br>⚠ 与 05 **互不相干**,各有版本号;本文是**领先于代码的实现规范**,固件尚未实现 |

## 工程位置

```
dev/app/sensor_beacon/
├── main.c                         # 应用主逻辑(状态机骨架)
└── pca10040e/s112/
    ├── armgcc/                    # GCC + Makefile 构建(已完整配置,主用)
    │   ├── Makefile
    │   └── sensor_beacon_gcc_nrf52.ld
    ├── config/sdk_config.h        # SDK 配置(已开启 SAADC 12bit)
    ├── ses/                       # SEGGER Embedded Studio 工程(由 beacon 复制,见 README)
    ├── arm4/ arm5_no_packs/       # Keil MDK 工程(由 beacon 复制)
    └── iar/                       # IAR 工程(由 beacon 复制)
```

> 详见工程内 [../app/sensor_beacon/README.md](../app/sensor_beacon/README.md)。
