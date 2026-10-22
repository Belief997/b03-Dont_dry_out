# tool —— 开发期工具脚本

⚠ **这些都不是工程的一部分。** 不参与任何构建，删掉不影响固件与上位机。
放在一起只是为了不散落在源码树里。

| 工具 | 干什么 | 怎么跑 |
|---|---|---|
| [`sizecheck/`](sizecheck/) | 量固件真实占用了多少 flash，还剩多少能加新功能 | `bash sizecheck/build.sh 1 && bash sizecheck/build.sh 0 && python sizecheck/report.py` |
| `solve_calibration.py` | 三通道联合标定：解 `W = Σ kᵢ·(xᵢ − zᵢ)` 的最小二乘/恰定解 | `python solve_calibration.py` |
| `scan_cn_strings.py` | 找出源码里**字符串字面量**中的中文（注释不算） | `python scan_cn_strings.py <文件...>` |
| `pinmap.py` | 列出 32 个 GPIO 的占用与空闲情况，全部从代码/配置反查 | `python pinmap.py` |

---

## sizecheck

回答"还剩多少 flash"。本机没装 Keil，所以拿不到 armcc 的权威数字，它用
`arm-none-eabi-gcc` 按同一份源文件清单、同一组宏、同一套内存布局**真实链接**一次
来测——比把 `.o` 大小加起来准得多。

详见 [`sizecheck/README.md`](sizecheck/README.md)，那里写了结果的可信度边界与踩过的坑。

## solve_calibration.py

标定模型是**三通道联合**的，不是逐通道增益：

```
W = k0·(x0 − z0) + k1·(x1 − z1) + k2·(x2 − z2)
```

⚠ `kᵢ` 是最小二乘解出的权重，**实测有一个是负数**（真实数据 `k2 = −2.555e-03`）。
不要对 `k` 的符号或量级做假设，也不要试图单独用某个 `kᵢ` 去换算单通道重量。

脚本里内置的那组数据是真实标定记录（3 个砝码 200/300/400 g，恰定求解），
固件侧的对应实现是 `app_storage_weight_calc()`，协议侧说明见
`note/06-ble-command-protocol.md` §5 的 `CMD_CAL_GET`。

需要 numpy。

## scan_cn_strings.py

按语言正确地跳过注释，只报**字符串字面量**里的中文——因为会经 UART/终端输出的
中文才可能乱码，注释不会。

支持 C / Rust / Dart。两个已修的坑值得留意，改这个脚本时别改回去：

- **Dart 用单引号做字符串**，C/Rust 用它做字符字面量，处理方式必须分开
  （靠扩展名判断）；
- **Rust 的生命周期标注 `'static` 也以引号开头且没有闭合引号**。当成字符字面量
  会一路吞到下一个引号，把中间所有字符串漏掉——最初就是这个 bug 让统计少报了
  23 处。

```bash
# 例：扫固件与上位机
python scan_cn_strings.py ../app/sensor_beacon/*.c ../app/sensor_beacon/services/*.[ch]
python scan_cn_strings.py --fix-list ../sensor_tool/lib/main.dart   # 只输出 path:line
```

⚠ 脚本自己的输出是中文的。它是开发期工具、只在终端跑，不受"代码里不出现中文"
那条约束管——那条针对的是固件与上位机的产物。

## pinmap.py

回答"还剩哪些 IO 能用"。**不读任何硬编码清单**，逐项从真实来源反查：

| 信息 | 来源 |
|---|---|
| 固件引脚宏 | `app/sensor_beacon/main.c`、`drivers/*.h` |
| UART 日志脚 | `config/sdk_config.h` 的 `NRF_LOG_BACKEND_UART_TX_PIN` |
| LFCLK 是否占 P0.00/01 | 同上的 `NRF_SDH_CLOCK_LF_SRC` |
| 复位脚 | `.uvprojx` 的 `CONFIG_GPIO_AS_PINRESET` + `system_nrf52810.c` 的 `PSELRESET` |
| DK 外设冲突 | `components/boards/pca10040.h` |

⚠ 表里的 `[AINn]` 标记**只是提示该脚可做模拟输入，不代表已占用**。本工程 SAADC
用的是 `NRF_SAADC_INPUT_VDD`（内部 VDD，不接引脚），所以 8 个 AIN 全部可自由支配
—— `main.c` 注释里"留给模拟输入"是预防性保留，并无对应需求（v0x02 协议已删掉
外部传感器电压字段）。

⚠ E73-TBM 那一列来自 `main.c` 注释里记录的实测调查，**不是**从文件反查的
（模块原理图不在仓库里）。换模块型号时要重新核。
