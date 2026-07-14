# 02 · 工具链 · 编译与烧录流程

## 1. 工具链选择

| 工具链 | 说明 | 例程目录 |
|--------|------|----------|
| **GCC + Make**(GNU Arm Embedded) | 免费、可命令行/CI、本项目**主用** | `armgcc/` |
| SEGGER Embedded Studio (SES) | 官方推荐 IDE,对 Nordic 免费 | `ses/` |
| Keil MDK (ARM) | 国内常用;52810 需 MDK5 + Pack | `arm5_no_packs/` `arm4/` |
| IAR EWARM | 商业 | `iar/` |

> 本工程的 **armgcc/Makefile 已完整配置好**(改名、链接脚本、SAADC 源文件均已就绪),推荐先用它跑通。其余 IDE 工程是从 beacon 例程复制而来,若要使用需自行补入 SAADC 源文件,详见工程 README。

### GCC 工具链安装(Windows)
1. **GNU Arm Embedded Toolchain**(arm-none-eabi-gcc)。SDK 期望版本记录在 `components/toolchain/gcc/Makefile.*`(15.0.0 对应 6-2017-q2 一带;新版一般也能编)。
2. **make**(Windows 可用 MSYS2 / GnuWin32 / Git-Bash 附带)。
3. 让 SDK 找到编译器:编辑 `components/toolchain/gcc/Makefile.windows`,把 `GNU_INSTALL_ROOT` 指向你的 arm-none-eabi 安装 `bin/` 目录(注意结尾斜杠)。

### 烧录/调试工具
- **nRF Command Line Tools**(内含 `nrfjprog` + J-Link),Makefile 的 `flash`/`erase` 目标直接调用它。
- 硬件:J-Link(DK 板载即是)或兼容调试器,SWD 连接。

## 2. 编译流程(GCC)

工作目录:`dev/app/sensor_beacon/pca10040e/s112/armgcc/`

```bash
make            # 编译,产物在 _build/(nrf52810_xxaa.hex / .out / .map)
make -j         # 并行编译
make clean      # 清理(删 _build)
```

编译产物:`_build/nrf52810_xxaa.hex`(应用固件,**不含** SoftDevice)。

## 3. 烧录流程(关键:分两部分)

BLE 固件 = **SoftDevice(协议栈)** + **Application(你的程序)**,两者烧到 Flash 的不同区域。

```
Flash 0x00000 ┌──────────────┐
              │  MBR         │  (SoftDevice 自带)
              │  SoftDevice  │  S112 ≈ 100KB
   0x19000    ├──────────────┤  ← 应用起始(见链接脚本 FLASH ORIGIN)
              │  Application │  ≤ 92KB
   0x30000    └──────────────┘  (192KB flash 末尾)
```

首次或换过 SoftDevice 时,按顺序:

```bash
# (可选)整片擦除,清掉旧内容
make erase                       # = nrfjprog -f nrf52 --eraseall

# 1) 先烧 SoftDevice(只需在换版本/擦除后做一次)
make flash_softdevice            # 烧 s112_nrf52_6.0.0_softdevice.hex

# 2) 再烧应用
make flash                       # 烧 _build/nrf52810_xxaa.hex(--sectorerase)并复位
```

之后日常只改应用代码时,重复 `make && make flash` 即可,**不必**重复烧 SoftDevice。

> 手动等价命令:
> ```bash
> nrfjprog -f nrf52 --program <softdevice.hex> --sectorerase
> nrfjprog -f nrf52 --program _build/nrf52810_xxaa.hex --sectorerase
> nrfjprog -f nrf52 --reset
> ```

## 4. 日志与调试(RTT)

- 本工程用 **SEGGER RTT** 作为 `NRF_LOG` 后端(不占 UART 引脚,深睡场景友好)。
- 查看日志:`JLinkRTTViewer`,或 `nRF Connect for Desktop` 的 RTT 终端;连接后选芯片 `NRF52810_XXAA`。
- `NRF_LOG_INFO(...)` 输出;进 System OFF 前工程会 `NRF_LOG_FLUSH()` 确保日志送出。
- **注意**:SWD 调试器连接时,`System OFF` 会被**仿真**——`sd_power_system_off()` / `SYSTEMOFF=1` 之后不会真正掉电,代码会停在其后的 `for(;;) __WFE();`。测真实功耗时必须**拔掉调试器**并断开供电重上电。

## 5. 功耗测量

- DK 上可用板载电流表(部分 DK)或串入万用表/Power Profiler Kit II(PPK2)测。
- 目标量级:System OFF ≈ 0.4µA;唤醒活动窗(采集 + 广播,1~2s+)mA 级瞬时。
- 详见 [04-solution-design.md](04-solution-design.md) 的功耗预算。
