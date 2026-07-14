# 03 · nRF52810 工程配置要点

本篇记录把 beacon 例程改造成 nRF52810 工程时**改了什么、为什么**,便于日后维护或迁移到真机。

## 1. 工程起点

以 `examples/ble_peripheral/ble_app_beacon/pca10040e/s112` 为基线复制到
`dev/app/sensor_beacon/pca10040e/s112`。选它的理由:非连接广播 + S112 + 52810 目标,与需求最接近。

## 2. 编译目标与宏(Makefile)

文件:`pca10040e/s112/armgcc/Makefile`

| 项 | 值 | 说明 |
|----|----|----|
| `TARGETS` | `nrf52810_xxaa` | 芯片型号 |
| `PROJECT_NAME` | `sensor_beacon_pca10040e_s112` | 已改名(原 ble_app_beacon_...) |
| `LINKER_SCRIPT` | `sensor_beacon_gcc_nrf52.ld` | 已随工程改名 |
| CFLAGS 关键宏 | `-DNRF52810_XXAA -DS112 -DSOFTDEVICE_PRESENT -DNRF_SD_BLE_API_VERSION=6` | 52810 + S112 |
| | `-DFLOAT_ABI_SOFT -mfloat-abi=soft -mcpu=cortex-m4` | **52810 无 FPU → 软浮点** |
| | `-DNRF52_PAN_74 -DSWI_DISABLE0 -DCONFIG_GPIO_AS_PINRESET` | 勘误/外设开关 |
| | `-DBOARD_PCA10040` | 板级头(DK 硬件) |
| | `-DDEVELOP_IN_NRF52832` | **见下方专门说明** |
| `__HEAP_SIZE` / `__STACK_SIZE` | 2048 / 2048 | 可按需调 |

### 为支持 ADC 新增的源文件
在 `SRC_FILES` 中已加入(beacon 原本没有):
```
$(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_saadc.c
$(SDK_ROOT)/integration/nrfx/legacy/nrf_drv_saadc.c
```
INC 路径 `modules/nrfx/drivers/include`、`modules/nrfx/hal`、`integration/nrfx/legacy` beacon 已包含,无需再加。

### ⚠ DEVELOP_IN_NRF52832 的含义
`pca10040e` 目标实际跑在 **nRF52832 DK** 硬件上,只是按 52810 内存/外设子集编译。该宏告诉代码"底层硅片其实是 52832",以套用 52832 的勘误处理。
- 在 **52832 DK / E73-TBA(若为 52832)** 上测试:**保留**。
- 烧到**真实 nRF52810** 芯片量产:应从 CFLAGS 与 ASMFLAGS 中**删除**这一行(Makefile 内已加注释提醒)。

## 3. 内存布局(链接脚本)

文件:`armgcc/sensor_beacon_gcc_nrf52.ld`

```
FLASH : ORIGIN = 0x19000,     LENGTH = 0x17000   # 应用区:S112 之后,92KB
RAM   : ORIGIN = 0x20001210,  LENGTH = 0x4df0    # 应用区:SD 占用之后
```
- `FLASH ORIGIN 0x19000`:S112 v6 占 0x00000~0x19000(约 100KB),应用紧随其后。
- `RAM ORIGIN 0x20001210`:S112 运行期占用低端 RAM,应用从此起。
- **RAM 起始可能随 BLE 配置变化**:`nrf_sdh_ble_enable()` 若返回 `NRF_ERROR_NO_MEM`,RTT 日志会打印"建议的 RAM 起始地址",据此调大 `ORIGIN`(并相应减 `LENGTH`)。本工程配置(1 个广播集、无连接)一般维持 0x20001210 附近。

## 4. sdk_config.h 改动

文件:`pca10040e/s112/config/sdk_config.h`

| 宏 | 原值 | 现值 | 说明 |
|----|------|------|------|
| `NRFX_SAADC_ENABLED` | 0 | **1** | 开启 nrfx SAADC 驱动 |
| `SAADC_ENABLED` | 0 | **1** | 开启 legacy 层(用 `nrf_drv_saadc` API 需要) |
| `NRFX_SAADC_CONFIG_RESOLUTION` | 1(10bit) | **2(12bit)** | 满量程 4096,`mV = raw*3600/4096` |
| `SAADC_CONFIG_RESOLUTION` | 1 | **2** | 同上(legacy) |
| `NRF_SDH_CLOCK_LF_SRC` | 1(XTAL) | 1(XTAL) | 板上有 32.768kHz 晶振,**维持 XTAL**,精度 20ppm |

其余保持 beacon 默认(app_timer / log-RTT / pwr_mgmt / BLE observer 等已开)。

## 5. 迁移到真实 nRF52810 硬件的清单

1. 删除 `DEVELOP_IN_NRF52832`(CFLAGS + ASMFLAGS)。
2. 若自定义板,替换 `BOARD_PCA10040` 为自己的板级头,或改用引脚宏直接定义(本工程已用裸引脚号,见 main.c 顶部 `WAKE_PIN` / `SENSOR_AIN`)。
3. 确认 `NRF_SDH_CLOCK_LF_SRC`:有晶振→XTAL(1);无晶振→RC(0)且需设 `NRF_SDH_CLOCK_LF_RC_CTIV`。
4. 核对唤醒引脚、传感器 AINx 通道、比较器输出类型(推挽/开漏 → 上下拉)。
5. 首次烧录 SoftDevice + App,拔调试器测真实功耗。
