# sensor_tool 框架说明

**本文只写"当前代码是怎么组织的"。** 分工如下，别在三处重复写同一件事：

| 想知道 | 看哪儿 |
|---|---|
| 怎么构建、路径坑、竖窗尺寸、**设备侧的硬约束** | [README.md](README.md) |
| 广播包/命令帧的**字节定义** | `dev/note/05-*.md`、`dev/note/06-*.md` |
| 代码分层、依赖方向、全局状态、数据通路、测试分层 | 本文 |

> 状态截止 2026-09-04。核对方式: `cargo test` 22 passed / `flutter analyze` 0 issues。

---

## 1. 一句话

**Rust 管蓝牙与协议编解码，Flutter 只做 UI。** 中间是 flutter_rust_bridge (FRB) 生成的胶水，不手写。

---

## 2. 分层与依赖方向

依赖**严格单向**，无环 —— 这是这套结构唯一需要守住的规则：

```
  Flutter UI (Dart)                        lib/main.dart
        │
        │  只调 lib/src/rust/api/*.dart(FRB 生成的门面)
        │  ⚠ Dart 侧不存在"另一条路"绕过它
  ══════╪══════════════ FRB 边界 ══════════════════════════
        ▼
  api/          rust/src/api/       DTO + 薄转换 + 跨 burst 去重
        │                          ⚠ 不放业务判断
        ├──────▶ ble/    rust/src/ble/     独占蓝牙适配器,
        │                                  扫描/连接互斥状态机
        │                                  (btleplug + 自持 tokio runtime)
        └──────▶ proto/  rust/src/proto/   纯字节编解码, 不碰 IO
                          ├── adv.rs   广播协议 v0x02(单向, 设备→空气)
                          └── cmd.rs   命令协议 v1  (双向, NUS 透传)
```

**`proto` 不认识 `ble`，`ble` 不认识 `api`，`api` 不认识 Dart。** 三条理由：

1. **可测性。** `proto` 不碰 IO，于是最容易写错又最难在设备上调试的那部分（多帧重组、24bit 符号扩展、丢帧检测）能在**没有真设备**的情况下全量单测。当前 22 条测试全在这一层。
2. **边界稳定。** FRB 只扫描 `crate::api`（见 `flutter_rust_bridge.yaml` 的 `rust_input`）。把内部类型直接摆进签名，内部每次改动都会触发 Dart 侧重新生成；多写一层 DTO 换来"内部随便改，DTO 不变 Dart 就不用动"。
3. **状态收敛。** 适配器是独占资源，状态只能有一份。全部收在 `ble::STATE` 里，`api` 与 Dart 都只能通过函数问，不能自己记一份。

---

## 3. 文件清单（哪些能手改）

| 路径 | 行 | 职责 | 手改 |
|---|---:|---|---|
| `lib/main.dart` | 398 | 全部 UI：扫描页 + 5 个私有 widget + 配色常量 | ✅ |
| `lib/src/rust/**` | 1107 | FRB 生成的 Dart 绑定（5 个文件） | ❌ codegen |
| `rust/src/lib.rs` | 25 | crate 根 + 分层图注释 | ✅ |
| `rust/src/api/mod.rs` | 8 | 生成入口，只 `pub mod` | ✅ |
| `rust/src/api/ble.rs` | 156 | `BleMode` / `BleAdvEvent` DTO、4 个导出函数、`DEDUP` | ✅ |
| `rust/src/api/simple.rs` | 10 | FRB 模板残留：`greet()` + `init_app()` | ⚠ 见下 |
| `rust/src/ble/mod.rs` | 389 | 适配器状态机、扫描循环、`RoundDedup` + 3 条测试 | ✅ |
| `rust/src/proto/mod.rs` | 17 | 两套协议的边界说明 | ✅ |
| `rust/src/proto/adv.rs` | 237 | 广播 v0x02 解析 + 6 条测试 | ✅ |
| `rust/src/proto/cmd.rs` | 587 | 命令 v1 帧层 + 重组器 + 13 条测试 | ✅ |
| `rust/src/frb_generated.rs` | 641 | FRB 生成 | ❌ codegen |
| `rust_builder/` | — | FRB/cargokit 生成的构建桥 | ❌ |
| `windows/runner/main.cpp` | 113 | 竖窗尺寸 + 工作区夹取 | ✅ |
| `windows/CMakeLists.txt` | — | `/utf-8`（不加则中文注释触发 C4819 + `/WX` 直接失败） | ✅ |
| `test/widget_test.dart` | 26 | 纯 widget 测试 | ✅ |
| `integration_test/simple_test.dart` | 46 | 加载 dll 的桥连通性测试 | ✅ |

> ⚠ `api/simple.rs` 的 `greet()` 是模板残留，但**别删** —— `integration_test` 靠它证明"FRB 桥通了"，这是能与 BLE 环境问题解耦的唯一一条断言。`init_app()` 带 `#[frb(init)]`，是 `RustLib.init()` 的钩子，删了会连不上。

---

## 4. Rust 侧的四个全局单例

整个 crate 的状态就这四个 `static`。**它们各自为什么必须是全局、用哪种锁，是这一层最容易踩错的地方**：

| 名字 | 位置 | 类型 | 为什么全局 / 关键约束 |
|---|---|---|---|
| `RT` | `ble/mod.rs:61` | `LazyLock<Runtime>` | btleplug 的 future 要 tokio reactor，而 **FRB 的执行器不是 tokio**，混用会 panic `there is no reactor running`。⚠ 必须 `multi_thread`：扫描任务长驻，单线程下会把线程占满，后续 `block_on` 的短调用永远排不上。 |
| `ADAPTER` | `ble/mod.rs:71` | `LazyLock<OnceCell<Adapter>>` | `Manager::new()` 在 Windows 上要跨进程问 WinRT，每次重建明显变慢，取一次缓存。 |
| `STATE` | `ble/mod.rs:78` | `LazyLock<Mutex<State>>` | 适配器归属（`Mode` + 扫描任务句柄）只能有一份。⚠ 用 **std 的 Mutex 而非 tokio 的**：临界区只做几个赋值，**绝不跨 `.await`**（跨 await 持有 std::Mutex 会在多线程 runtime 上死锁）。 |
| `DEDUP` | `api/ble.rs:75` | `Mutex<Option<RoundDedup>>` | 一轮 burst 的多个包分散在多个回调里到达，去重表活不过单次回调。⚠ 也**不能**放进 `scan_start` 的闭包：那样每次重启扫描就重置，而设备的 `counter` 不会重置 → 重启后第一包会被误判成重复而丢掉。 |

`Mode` 是三态且**互斥**：`Idle` / `Scanning` / `Connected`。互斥不是实现偷懒，是设备侧约束（S112 只有 1 个广播集 + 1 个连接槽，且连接期间不播数据广播）——细节见 README 第 2 条与 `ble/mod.rs` 文件头。

---

## 5. 数据通路：一次扫描的完整链路

```
 Dart   _toggleScan()  ──▶ bleScanStart()               main.dart:_ScanPageState
   │                          │
   │                          ▼
 Rust  api::ble_scan_start(sink)                        api/ble.rs
   │      │ 建 DEDUP(若无)
   │      ▼
   │    ble::scan_start(闭包)                           ble/mod.rs
   │      │ 检查 Mode: Scanning→幂等返回 / Connected→报错 / Idle→继续
   │      ▼
   │    RT.spawn(scan_loop)
   │      │ ① central.events()      ⚠ 必须先订阅
   │      │ ② central.start_scan()  ⚠ 再开扫描 —— 反了会丢掉这中间的所有广播
   │      ▼  (btleplug 的 broadcast 通道对"当时无订阅者"的消息直接丢弃)
   │    while events.next()
   │      │
   │      ├─ extract()      三种事件都要处理: ManufacturerDataAdvertisement /
   │      │                 DeviceDiscovered / DeviceUpdated。只监听第一种会漏,
   │      │                 因为各平台后端的发法不同。带内联数据的直接用,
   │      │                 不带的回本地缓存取。
   │      ├─ quick_props()  取 MAC + RSSI。读的是 btleplug 本地缓存(非跨进程),
   │      │                 实测 0.1ms/事件, 在事件循环里调是安全的。
   │      ├─ adv::parse()   BadMagic → continue(环境里上百个设备也在用 0xFFFF,
   │      │                 不丢会把列表刷满); 其余错误照样上报, 否则固件升版本后
   │      │                 工具"什么都收不到"却毫无线索。
   │      ▼
   │    闭包(在 api 层)
   │      │ DEDUP.is_new_round(device_id, counter) → false 就 return, 不过 FFI
   │      ▼
   │    sink.add(BleAdvEvent)     sink 已关闭时返回 Err, 忽略即可
   ▼
 Dart  stream.listen → _events.insert(0, ev) → setState  上限 200 条
```

**一轮 burst 在 Dart 侧只出现一条。** 固件一轮播 `BLE_BEACON_ADV_EVENTS`(=15) 个广播事件 × 3 信道 = 最多 45 个内容相同的包，`counter` 在一轮内不变，去重就按它做。

---

## 6. FRB 边界的六条规则

1. `rust_input: crate::api` —— **只有 `rust/src/api/` 下的公开项**会生成 Dart。
2. `StreamSink` 从 **`crate::frb_generated`** 导入，不是 `flutter_rust_bridge` 根（FRB 2.x 挪过位置，写错会报 `not found in flutter_rust_bridge`）。
3. 改了 `api/` 下的**签名**必须 `flutter_rust_bridge_codegen generate`；只改函数体不用。
4. **`cargo test` 要在 codegen 之后跑** —— 新增 DTO 在 codegen 之前没有 `SseEncode` 实现，会编译失败。
5. DTO 里用 `Vec<i32>` 而不是 `[i32; 3]`（FRB 对定长数组支持不如 Vec）。协议层保持定长以表达"就是 3 通道"，转换只在 `api` 这一处。
6. 扫描回调必须 `Send + **Sync**`：循环里它以 `&F` 跨过 `.await`，而 `&F: Send` 要求 `F: Sync`。

---

## 7. UI 层结构（`lib/main.dart`）

```
SensorToolApp (MaterialApp, 主题在这里)
└── ScanPage / _ScanPageState        ← 全部可变状态: _adapter, _adapterError,
    │                                  _scanning, _events(上限 200)
    ├── _AdapterCard   适配器名 / 错误
    ├── _ScanControl   开始·停止 + 轮数
    ├── _EmptyHint     ⚠ "设备平时静默"提示 —— 有测试钉住, 别删
    └── _AdvTile       一轮广播; 解析失败走另一分支
        └── _chRow     ch0/ch1/ch2 + 增益标注(等宽字体对齐是必需的)
```

状态全在 `_ScanPageState` 里，没有状态管理框架 —— 当前只有一页，引进来不划算。加第二页（标定/记录）时再考虑，落点是把 `_events` 之类的数据挪到一个 `ChangeNotifier`。

### 配色约定（白底）

主题是 `Brightness.light` + `scaffoldBackgroundColor: Colors.white`。两个不显然的点：

- **必须显式指定纯白**：M3 light 默认拿带色调的 `surface` 当页面底色（teal 种子下算出来偏青的灰白），只翻 `brightness` 得不到白背景。
- **`Card` 刻意不跟着设成纯白**：页面已是纯白，卡片再纯白就与背景糊在一起，只剩阴影可辨。保持 M3 默认的 `surfaceContainerLow`。

文件顶部有 6 个语义色常量，**不要再往这个文件写 `Colors.whiteNN` 或 `Colors.*Accent`**：

| 常量 | 值 | 用途 |
|---|---|---|
| `_muted` | `0xFF5F6368` | 次要文字：RSSI、ID/电量行、空列表提示 |
| `_faint` | `0xFF9AA0A6` | 三级文字：通道名、增益标注 |
| `_ghost` | `0xFFC4C7C5` | 空列表的大图标 |
| `_ok` | `0xFF00695C` | teal 800，适配器可用 |
| `_danger` | `0xFFC62828` | red 800，适配器不可用 |
| `_warn` | `0xFFB26A00` | amber 900，广播解析失败 |

⚠ 原因值得记一下：`Colors.white24/38/54/70` 只在深色背景下成立，换白底后变成白字白底 —— 内容在界面上直接消失，**布局照旧、测试照过、不报任何错**，是最难发现的一类回归。`*Accent` 系是配深色背景的高亮色，白底上对比度不足（`orangeAccent` 几乎看不清）。

---

## 8. 测试分三层（这个划分是被迫的，不是洁癖）

| 层 | 命令 | 数量 | 能测什么 | 硬限制 |
|---|---|---:|---|---|
| Rust 单测 | `cd rust && cargo test` | **22** | 协议编解码全部（`adv` 6 + `cmd` 13）+ `ble` 的纯逻辑部分（去重/初始态 3） | 不需要设备，也不需要蓝牙 |
| Dart widget | `flutter test` | 1 | 不依赖 Rust 的展示型 widget | ⚠ `RustLib.init()` 需要真 native 库，`flutter_test` 环境里没有 → **不能构造 `SensorToolApp`**（它的 `initState` 会调 `bleAdapterName()`） |
| 集成 | `flutter test integration_test/simple_test.dart -d windows` | 3 | FRB 桥连通、`Mode` 起始为 Idle、适配器可枚举 | 要真 Windows 设备 + 蓝牙 |

⚠ **不要在集成测试里测"能不能扫到设备"**：设备平时完全静默，且 Windows 收包率天然很低。那属于需要真硬件在场的手工验证。

---

## 9. 当前能力边界

**已通**：FRB 桥、广播 v0x02 解析、命令 v1 帧层与重组、适配器枚举与扫描、轮次去重、竖窗约束、白底主题。

**未通**：

| 缺口 | 落点 | 前置 |
|---|---|---|
| **GATT 连接** —— 整条路上最大的未验证点，一次都没跑过 | `ble/mod.rs`（加 connect/notify，`Mode::Connected` 目前是死枚举值） | 无，可立即做 |
| 各命令的载荷结构解析 | 新建 `proto/payload.rs` | 固件先实现命令，否则无从实测 |
| 串口通路（唯一能长时间连续观测的通路） | 新建 `serial/`，`serialport` crate | 无 |
| 最小二乘标定向导 / 记录下载 CSV / 配置面板 | `lib/` 加页面 | 固件的 `CMD_INFO` + `CMD_REC_READ` |

⚠ **固件侧硬前置**：note 06 §12 清单里至少要先有 `CMD_INFO` 与 `CMD_REC_READ`，连接层才有东西可测。当前固件的 `nus_data_handler` 只做 hexdump + 原样回显 —— `proto/cmd.rs` 有一条 `rejects_echo_from_current_firmware` 专门覆盖这个现状，免得把回显误当响应解。

---

## 10. 加东西时的落点速查

| 要加的东西 | 放哪 | 别放哪 |
|---|---|---|
| 新的字节格式 / 位域 | `proto/` | 别放 `api/`（会失去单测覆盖） |
| 新的 BLE 动作（连接、订阅、写特性） | `ble/` | 别放 `api/`（状态会分裂成两份） |
| 新的 Dart 可见类型或函数 | `api/` + 跑 codegen | 别直接暴露内部类型 |
| 新的界面 | `lib/` | 别在 Dart 侧重实现协议解析 |
| 判断"什么时候该做什么" | `lib/`（UI）或 `ble/`（受硬件约束的时序） | 别放 `proto/` |
