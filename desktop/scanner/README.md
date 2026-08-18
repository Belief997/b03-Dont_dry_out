# sensor-beacon-scanner

`sensor_beacon` 固件的 BLE 广播扫描与解析工具(Rust, 跨平台: Windows / macOS / Linux)。

扫描本机蓝牙适配器收到的广播，过滤 Company ID `0xFFFF` 的厂商自定义段，
按协议 **v0x02** 解析 16 字节载荷并打印。

## 广播格式(与固件 main.c / ble_beacon.h 对齐)

空中结构: `Flags(3B)` + `厂商自定义段头(4B: len+type+CompanyID)` + `载荷(16B)`

| 偏移 | 长度 | 内容 |
|------|------|------|
| 0 | 1 | 魔数 `0xAB` |
| 1 | 1 | 版本 `0x02` |
| 2..4 | 2 | 设备 ID(小端, FICR->DEVICEID 低 16bit) |
| 4 | 1 | 计数器(单击次数, 8bit) |
| 5..7 | 2 | 电池电压(mV, 小端) |
| 7..10 | 3 | 通道 0 = HX711#1 chA(增益 128), 有符号 24bit 小端 |
| 10..13 | 3 | 通道 1 = HX711#1 chB(增益 32), 有符号 24bit 小端 |
| 13..16 | 3 | 通道 2 = HX711#2 chA(增益 128), 有符号 24bit 小端 |

广播行为: 设备平时静默；单击按键后按 100ms 间隔连播 3 个广播事件
(3 信道 × 3 次 = 9 个空中包)，内容相同。本程序按 `(设备ID, 计数器)`
去重，每轮只打印第 1 个包。

## 安装 Rust

```powershell
winget install Rustlang.Rustup
# 或 https://rustup.rs 下载 rustup-init.exe
```

安装后新开终端确认:

```powershell
cargo --version
```

## 构建

```powershell
cd ddo/desktop/scanner
cargo build --release
```

## 运行

```powershell
cargo run --release
```

或直接运行 `target\release\sensor-beacon-scanner.exe`。

## 输出示例

```
适配器: Generic Bluetooth Adapter
开始扫描... 过滤 Company ID 0xFFFF, 协议魔数 0xAB 版本 0x02
==============================
MAC       : E4:5F:01:xx:xx:xx
RSSI      : -45 dBm
设备ID    : 0x1A2B (6699)
计数器    : 7
电池      : 3012 mV
ch0(#1 chA, 增益128) =   591432
ch1(#1 chB, 增益 32) =    36650
ch2(#2 chA, 增益128) =  -105985
```

## 常见问题

- **找不到适配器**: 确认系统蓝牙已开启(Windows: 设置 → 蓝牙和其他设备)。
- **扫描不到**: 设备平时不播广播 —— 单击设备按键触发一轮广播后再看。
  软件过滤了 Company ID `0xFFFF`，其他设备广播不会显示。
- **Windows 报错**: 需 Windows 10 1709+，蓝牙驱动正常(btleplug 走 WinRT
  BLE 侦听器，无需配对)。
- **Linux**: 需要 `libdbus-1-dev`(Debian/Ubuntu: `sudo apt install libdbus-1-dev pkg-config`)。
