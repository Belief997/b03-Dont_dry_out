# sensor_beacon

nRF52810 + S112 低功耗事件触发广播上报工程骨架。
方案与背景见 [`dev/note/`](../../note/README.md)。

## 目录结构

```
sensor_beacon/
├── main.c                              # 应用主逻辑(状态机骨架,含 TODO)
└── pca10040e/s112/
    ├── armgcc/                         # ★ GCC/Make 构建(已完整配置,推荐)
    │   ├── Makefile                    #   PROJECT_NAME/链接脚本已改名,已加 SAADC 源
    │   └── sensor_beacon_gcc_nrf52.ld
    ├── config/sdk_config.h             # 已开启 SAADC(nrfx+legacy,12bit)
    ├── ses/                            # SES 工程(自 beacon 复制,见"其他 IDE")
    ├── arm4/  arm5_no_packs/           # Keil 工程(自 beacon 复制)
    └── iar/                            # IAR 工程(自 beacon 复制)
```

## 快速开始(GCC,推荐)

前置:GNU Arm Embedded 工具链、make、nRF Command Line Tools(nrfjprog + J-Link)。
先按 [`dev/note/02`](../../note/02-toolchain-and-build-flow.md) 配好 `components/toolchain/gcc/Makefile.windows` 的编译器路径。

```bash
cd pca10040e/s112/armgcc

make                     # 编译 → _build/nrf52810_xxaa.hex

make flash_softdevice    # 首次/换版本:烧 S112 v6.0.0
make flash               # 烧应用并复位
```

日志:用 JLink RTT Viewer 或 nRF Connect 的 RTT 终端查看(芯片选 `NRF52810_XXAA`)。
> 调试器连接时 System OFF 被仿真、不会真掉电;测真实功耗请拔调试器。

## 当前配置(集中在 main.c 顶部)

| 宏 | 当前值 | 说明 |
|----|--------|------|
| `WAKE_PIN` | **13** (P0.13) | 比较器输出 GPIO;当前为 DK Button1 便于台架测试,上真机时需改 |
| `WAKE_PIN_PULL` | **`NRF_GPIO_PIN_PULLUP`** | 使能内部上拉 |
| `SENSOR_AIN` | **`NRF_SAADC_INPUT_AIN0`** (P0.02) | 传感器模拟输入 |
| `SAADC_CH_SENSOR` / `SAADC_CH_BATTERY` | 0 / 1 | 传感器通道 / 电池通道(内部 VDD) |
| `SETTLING_TIME_MS` | **1500** | 传感器上电稳定等待时间(ms) |
| `ADV_INTERVAL_MS` / `ADV_DURATION_MS` | **100 ms** / **2000 ms** | 广播间隔与总时长 |
| `ADV_TX_POWER_DBM` | **0 dBm** | 广播发射功率 |
| `APP_COMPANY_IDENTIFIER` | **0xFFFF** | SIG 保留 ID(测试用) |
| `DEVICE_ID_LEN` | **2** | 广播中设备 ID 字节数 |

### ADC 参考配置

- 参考源: **内部 0.6V** (`NRF_SAADC_REFERENCE_INTERNAL`)
- 增益: **1/6** (`NRF_SAADC_GAIN1_6`),满量程 = 0.6V × 6 = **3.6V**
- 分辨率: **12 位**(`sdk_config.h` 中配置)

### 上真机前仍需确认

- `WAKE_PIN` —— 改为实际布线的 GPIO 引脚号
- `SETTLING_TIME_MS` —— 按传感器手册调整
- `ADV_INTERVAL_MS` / `ADV_DURATION_MS` / `ADV_TX_POWER_DBM` —— 按功耗/可靠性折中调整
- `APP_COMPANY_IDENTIFIER` —— 若有 SIG 分配的 Company ID 请替换
- `DEVICE_ID_LEN` —— 按需 2 或 4 字节

迁真机(真实 52810 硅片)时,记得从 `armgcc/Makefile` 删除 `DEVELOP_IN_NRF52832`(见 [`dev/note/03`](../../note/03-nrf52810-project-config.md))。

## 行为概述

常驻 System OFF;比较器上升沿经 GPIO SENSE 唤醒(复位)。唤醒后读引脚电平:
- **高电平(事件)**:计数器 +1 → 等 1~2s 传感器稳定 → SAADC 采集(传感器 + 电池)
  → 非连接广播上报 → 武装 SENSE=LOW → System OFF。
- **低电平(信号回落/冷启动)**:不广播 → 武装 SENSE=HIGH → System OFF。

8bit 计数器存于 GPREGRET,跨睡眠/复位保持。详见 [`dev/note/04`](../../note/04-solution-design.md)。

广播负载的字节结构与网关解析(Python/C 示例、去重策略)见对接协议
[`dev/note/05`](../../note/05-broadcast-payload-protocol.md)。

## 其他 IDE(SES / Keil / IAR)

`ses/` `arm4/` `arm5_no_packs/` `iar/` 是从 beacon 例程**原样复制**的,存在两点差异,若要使用需手动处理:

1. **工程内部名仍是 `ble_app_beacon_...`**(仅命名,不影响功能)。
2. **未包含 SAADC 源文件**——需在工程的源文件列表中补入:
   - `modules/nrfx/drivers/src/nrfx_saadc.c`
   - `integration/nrfx/legacy/nrf_drv_saadc.c`

`config/sdk_config.h`(已开 SAADC)与 `main.c` 为各 IDE 共用,补入上述源文件后即可编译。
若只用 GCC,可忽略这些目录。
