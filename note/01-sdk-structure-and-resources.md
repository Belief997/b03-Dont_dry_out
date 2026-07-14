# 01 · SDK 结构 · 芯片与资源

## 1. nRF5 SDK 15.0.0 目录结构

```
nRF5_SDK_15.0.0_a53641a/
├── components/            # SDK 核心
│   ├── ble/               # BLE 协议层封装:common(ble_advdata 等)、
│   │                      #   ble_services/(各 GATT 服务)、ble_advertising、peer_manager
│   ├── softdevice/        # SoftDevice 头文件 + hex + 接入层(common/ = nrf_sdh*)
│   │   ├── s112/ s132/ s140/ s212/
│   │   └── common/        # nrf_sdh.c / nrf_sdh_ble.c / nrf_sdh_soc.c(SoftDevice Handler)
│   ├── libraries/         # 通用库:app_timer、pwr_mgmt、log、bsp、button、fds、fstorage...
│   ├── boards/            # 板级定义 boards.c + 各 pcaXXXXX.h
│   ├── drivers_nrf/       # 旧版驱动残留(多数已迁移到 nrfx)
│   └── toolchain/         # cmsis、gcc/Makefile 模板、启动依赖
├── modules/nrfx/          # nrfx —— 新一代寄存器 HAL + 外设驱动(15.x 主力)
│   ├── hal/               # nrf_gpio.h / nrf_saadc.h ... 寄存器内联封装
│   ├── drivers/           # nrfx_saadc.c / nrfx_clock.c / nrfx_gpiote.c ... 驱动实现
│   ├── mdk/               # 芯片头文件、startup、system_*、链接公共段(nrf_common.ld)
│   └── soc/
├── integration/nrfx/      # nrfx 与旧 API 的桥接
│   └── legacy/            # nrf_drv_*.c/h(把旧 nrf_drv_ 名字转发到 nrfx_)
├── examples/              # 例程(最重要的学习/起步资源)
│   ├── ble_peripheral/    # 外设角色:ble_app_beacon(本项目基线)、ble_app_hrs...
│   ├── ble_central/       # 主机角色
│   ├── peripheral/        # 非 BLE 外设:saadc、gpiote、pwm、timer、rtc...
│   └── dfu/               # 固件升级
├── external/              # 第三方:segger_rtt(RTT 日志)、fprintf、freertos、micro-ecc...
├── config/                # 全局 sdk_config 模板
├── documentation/         # 离线文档(index.html 打开)
└── license.txt
```

### 两套驱动 API 的关系(常见困惑点)

- **nrfx**(`modules/nrfx/drivers`):新 API,函数/宏前缀 `nrfx_`。SDK 15 的主力。
- **legacy**(`integration/nrfx/legacy`):旧 API,前缀 `nrf_drv_`,内部只是 `#define nrf_drv_xxx nrfx_xxx` 转发。老例程仍在用。
- 两者可混用,但每个外设的开关在 `sdk_config.h` 里**各有一个宏**:`NRFX_SAADC_ENABLED`(nrfx 层)与 `SAADC_ENABLED`(legacy 层)。用 legacy API 时两者都要开。

## 2. 支持的芯片与测试板

### 本 SDK(15.0.0)支持的芯片
由 `modules/nrfx/mdk` 的 startup 文件可确认:**nRF52810 / nRF52832 / nRF52840**。
(注:nRF52811 在 SDK 15.2 才加入;nRF51 系列停留在 SDK 12.x,15.x 已不支持。)

### 常见官方测试板(DK)
| 板卡 | 芯片 | 例程目录后缀 |
|------|------|--------------|
| PCA10040 | nRF52832 | `pca10040` |
| **PCA10040e** | 在 PCA10040(52832)上**仿真 nRF52810** | `pca10040e` |
| PCA10056 | nRF52840 | `pca10056` |
| PCA10059 | nRF52840 Dongle | `pca10059` |

> **pca10040e 是本项目使用的目标**:它用 52832 DK 硬件,但按 52810 的内存/外设子集来编译,是官方推荐的 52810 开发/验证方式。E73-TBA/TBB 模组上板测试同理适用。

## 3. SoftDevice 选型

本 SDK 内含:`s112 / s132 / s140 / s212(ANT)`。

| SoftDevice | 角色 | 典型芯片 | 说明 |
|-----------|------|----------|------|
| **S112** | 外设 + 广播者(**不能做主机/不能发起连接**) | 52810 / 52832 | 体积小(~100KB flash),**本项目所用**;广播上报场景足够 |
| S132 | 外设 + 主机 | 52832 | 需要连接/中心角色时用 |
| S140 | 外设 + 主机 + Coded PHY/长距离 | 52840 | 全功能 |
| S212 | ANT | — | ANT 协议 |

本项目为**非连接广播**上报,只需广播者角色 → **S112 v6.0.0**(`components/softdevice/s112/hex/s112_nrf52_6.0.0_softdevice.hex`),`NRF_SD_BLE_API_VERSION=6`。

## 4. nRF52810 资源要点

- 内核:Cortex-M4 @ 64MHz,**无 FPU**(必须软浮点 `-mfloat-abi=soft`,SoftDevice 也是 soft 变体)。
- 存储:**192KB Flash / 24KB RAM**(三款里最小,需精打细算)。
- 无线:BLE 5,支持 1Mbps / 2Mbps;**不支持** Coded PHY(长距离)。
- 模拟:**SAADC 8 通道 / 12bit**(含内部 VDD 输入,可测电源电压,无需外部分压引脚)。
- **无 LPCOMP**(经确认);低功耗电平比较需外部电路(本项目正是外部比较器方案)。
- 低功耗:System OFF ~0.4µA;System ON idle ~1.5µA。
- 其余外设(UARTE/SPIM/TWIM/PWM/PDM/RTC/TIMER/GPIOTE/TEMP/RNG/WDT 等)数量以《nRF52810 Product Specification》为准。

### 三款芯片对比

| | **nRF52810** | nRF52832 | nRF52840 |
|---|---|---|---|
| 内核 | M4(无 FPU) | M4F | M4F |
| Flash | 192KB | 512KB | 1MB |
| RAM | 24KB | 64KB | 256KB |
| BLE | 1M/2M | 1M/2M | 1M/2M + Coded/长距离 |
| USB | 无 | 无 | 有 |
| 加密协处理器 | 无 | 无 | 有(CC310) |
| 典型 SoftDevice | S112 | S112 / S132 | S140 |
| 定位 | 成本优先 | 均衡 | 全功能 |

> 结论:本项目"深睡 + GPIO 唤醒 + ADC 采集 + 非连接广播"需求,nRF52810 + S112 完全够用。
