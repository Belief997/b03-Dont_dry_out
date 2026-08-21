# sensor-beacon-scanner

`sensor_beacon` 固件的 BLE 广播扫描与解析工具(Rust, 跨平台: Windows / macOS / Linux)。

扫描本机蓝牙适配器收到的广播，过滤 Company ID `0xFFFF` 的厂商自定义段，
按协议 **v0x02** 解析 16 字节载荷并打印，同时统计每轮的收包率。

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

广播行为: 设备平时静默；单击按键后按 `BLE_BEACON_ADV_INTERVAL_MS`(当前 100ms)
间隔连播 `BLE_BEACON_ADV_EVENTS`(当前 **15**)个广播事件，每个事件在 3 个信道
各发 1 包 → 理论上限 **45 个空中包**，内容全部相同。

本程序按 `(设备ID, 计数器)` 去重：每轮只完整打印第 1 个包，其余重复包累加计数，
并在一轮结束时给出**收包率**。

> ⚠ `--events` 的默认值(15)必须与固件的 `BLE_BEACON_ADV_EVENTS` 一致，
> 否则收包率的分母是错的。改了固件记得同步。

## 构建与运行

```powershell
cd dev/desktop/scanner
cargo build --release
cargo run --release
```

或直接运行 `target\release\sensor-beacon-scanner.exe`。

> ⚠ `.cargo/config.toml` 把 `target-dir` 指到了 `F:/rs_t`(为绕开 Windows 260
> 字符路径限制)。若你机器上没有 F: 盘，构建会报
> `系统找不到指定的路径 (os error 3)`。临时绕过:
> `CARGO_TARGET_DIR=D:/some/short/path cargo build --release`

### 命令行选项

```
-e, --events N   一轮的广播事件数(默认 15, 须与固件 BLE_BEACON_ADV_EVENTS 一致)
-v, --verbose    打印每个重复包(默认只在行内刷新计数, 不滚屏)
-h, --help       显示帮助
```

## 输出示例

```
适配器: WinRT
过滤: Company ID 0xFFFF, 魔数 0xAB, 版本 0x02
一轮理论包数: 15 事件 × 3 信道 = 45 包(用 --events 改)
设备平时静默 —— 单击设备按键后才有数据。Ctrl+C 退出。

==============================
MAC       : E4:5F:01:xx:xx:xx
RSSI      : -45 dBm
设备ID    : 0x1A2B (6699)
计数器    : 7
电池      : 3012 mV
ch0(#1 chA, 增益128) =   591432
ch1(#1 chB, 增益 32) =    36650
ch2(#2 chA, 增益128) =  -105985
  收包 3/45 ...
  └─ 设备 0x1A2B 计数器 7 一轮结束: 收到 3/45 包 (7%), 历时 1.4s, RSSI -47..-45 dBm
     ⚠ 收包率偏低。可尝试: 靠近设备 / 关掉其它蓝牙设备 / 见 README「漏收」一节
```

## 漏收问题 —— 实测结论

**先看这一节再动手改代码。** 三个诊断工具的实测数据推翻了两个看起来最像
元凶的猜想，真正的原因在系统层。

### 已排除: btleplug 的 16 格事件队列溢出

队列确实只有 16 格(`common/adapter_manager.rs:34` `broadcast::channel(16)`)，
且滞后时 `BroadcastStream` 产出的 `Err(Lagged(n))` 会被 `.ok()` **静默丢弃**
(同文件 `:58`) —— 读代码时非常像元凶。

但 `cargo run --release --bin ab` 的 A/B 实测否掉了它。两个订阅者同时消费
同一事件源，一端不阻塞、一端复刻旧版的 `peripherals().await` +
`properties().await`：

```
fast(不阻塞)      收到  1191 个事件
slow(旧版做法)    收到  1191 个事件
slow 比 fast 少       0 个 (0.0% 丢失)
slow 累计阻塞在查属性上: 149 ms (平均每事件 0.1 ms)
```

原因是这些调用读的都是**本地缓存**(DashMap + RwLock)，不是跨进程 WinRT 调用。
所以"主循环里别做慢操作"在这里不是修复，只是良好习惯。

### 已排除: Windows 合并内容相同的广播

固件一轮里 15 个包载荷完全相同，很容易怀疑被系统按内容去重。
`cargo run --release --bin dupchk` 实测：30 秒内 670 个包中有 **614 个与前一包
字节完全相同**，却仍被逐包上报。Windows 不做内容去重，这条不成立。

### 真正原因: 系统扫描窗口太稀疏

`dupchk` 同时统计了广播到达的间隔分布(环境里 100+ 个 BLE 设备，样本 670 包)：

```
最小 2ms, 中位 1346ms, P90 4377ms
间隔 <100ms 的仅占 4.7%
```

即 Windows 的 BLE 侦听器大约**每秒才开一次扫描窗口**。而固件一轮只持续
15 × 100ms = **1.5 秒**，期间只有 1~2 个窗口会打开 —— 45 个空中包里能被采到的
本来就只有一两个。

这不是 btleplug 或本程序的问题：Windows 不暴露 scan window / interval 的设置
(btleplug `watcher.rs:47` 只能设 Active/Passive 模式)，应用层无法把窗口调密。

`cargo run --release --bin diag` 可以看环境有多吵，本机实测 **78 事件/秒、
146 个设备**同时在播。

### 改善手段(在固件侧，比扫描端调优有效得多)

**a) 把一轮的时间拉长，让它横跨更多扫描窗口。** 两种做法等价，选一个改
`ble_beacon.h`：

- `BLE_BEACON_ADV_INTERVAL_MS`: 100 → **300~500**
- 或 `BLE_BEACON_ADV_EVENTS`: 15 → **40~50**

按实测的 ~1.3 秒窗口周期，一轮至少要持续 4~5 秒才能稳定命中 3~4 次。
代价是功耗和"按一下要等多久才出数"的体验。

**b) 在载荷里加"本轮第几包"的序号**(1 字节；当前载荷 16 字节，上限 24，有余量)。
这不提高收包率，但能让漏收可诊断 —— 直接知道漏的是第几包。代价是
"一轮里的包完全一样"这条需求要放宽。

## 诊断工具

三个独立的 bin，都在 `src/bin/` 下：

| 命令 | 用途 |
|------|------|
| `cargo run --release --bin diag [秒数]` | 不做过滤，统计全部 BLE 事件速率、设备数、Company ID 分布。看环境有多吵。 |
| `cargo run --release --bin dupchk [秒数]` | 统计相同内容的包是否被逐包上报，以及**到达间隔分布**(判断扫描窗口密度的关键)。 |
| `cargo run --release --bin ab [秒数]` | A/B 对比"主循环阻塞"与"不阻塞"各能收到多少事件，验证队列溢出假设。 |

## 常见问题

- **找不到适配器**: 确认系统蓝牙已开启(Windows: 设置 → 蓝牙和其他设备)。
- **扫描不到**: 设备平时不播广播 —— 单击设备按键触发一轮后再看。
  软件过滤了 Company ID `0xFFFF`，其他设备的广播不显示(想看全部用 `diag`)。
- **收包率很低**: 这是已知的系统限制，见上面「漏收问题」一节。
- **构建报找不到路径**: 见上面 `.cargo/config.toml` 的说明。
- **Windows 报错**: 需 Windows 10 1709+，蓝牙驱动正常(btleplug 走 WinRT
  BLE 侦听器，无需配对)。
- **Linux**: 需要 `libdbus-1-dev`(Debian/Ubuntu: `sudo apt install libdbus-1-dev pkg-config`)。
