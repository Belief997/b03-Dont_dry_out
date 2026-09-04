# sensor_tool

`sensor_beacon` 设备的桌面工具: BLE 扫描 / 连接、数据采集与标定配置。
**Rust 管蓝牙与协议编解码, Flutter 只做 UI。** 当前只做 Windows 桌面, 竖窗布局。

```
lib/                    Flutter UI (Dart)
  main.dart               扫描页(当前唯一页面)
  src/rust/               ⚠ FRB 生成, 不要手改
rust/                   Rust core
  src/api/                FRB 边界: DTO + 薄转换
  src/ble/                独占蓝牙适配器, 扫描/连接互斥状态机 (btleplug)
  src/proto/              纯字节编解码, 不碰 IO, 全部可 cargo test
    adv.rs                  广播协议 v0x02  → dev/note/05-*.md
    cmd.rs                  命令协议 v1     → dev/note/06-*.md
rust_builder/           ⚠ FRB/cargokit 生成的构建桥, 不要手改
windows/runner/         Win32 宿主 (竖窗尺寸与最小尺寸在这里)
```

---

## 构建与运行

```bash
flutter pub get
cd rust && cargo test && cd ..        # 协议层单测, 不需要真设备
flutter run -d windows
```

改了 `rust/src/api/` 下的**签名**之后必须重新生成 Dart 绑定:

```bash
flutter_rust_bridge_codegen generate
```

只改函数体不用重新生成。**`cargo test` 必须在 codegen 之后跑** —— 新增的 DTO
在 codegen 之前没有 `SseEncode` 实现, 会编译失败。

工具链版本(已验证): Flutter 3.44.4 / Dart 3.12.2 / cargo 1.97.0 /
flutter_rust_bridge_codegen 2.13.0 / VS Community 2022 17.14.23。

---

## ⚠ 路径约束(踩过的坑, 别再踩)

**Flutter 的 Windows 构建拒绝路径中含 `'#!$^&*=|,;<>?` 里的任何字符。**
本工程原计划放在
`D:\DocDev\nrf\E73-TBA & E73-TBB 开发资料\...\dev\sensor_tool`,
因为祖先目录名里有一个 `&` 而**完全无法构建**:

```
Path ... contains invalid characters in "'#!$^&*=|,;<>?".
Please rename your directory ...
```

已实测的两条结论:

- **目录联接(junction)绕不过去。** 建了 `D:\sbt` → 深路径的 junction,
  从 `D:\sbt\sensor_tool` 构建, 报错里打的仍是解析后的真实路径。
- **重命名祖先目录不能在 Claude Code 会话里做。** 会话自身的工作目录在那棵树里,
  构成祖先锁; VS Code 的文件监视器也持有句柄。必须关掉这些再从树外操作。

**最终采用的方案**: 整棵 SDK 树迁到 `D:\RemotePrj\nRF5_SDK_15.0.0_a53641a` ——
路径里既没有 `&` 也没有中文, 长度也短得多。顺带解决了固件侧那个
"`arm-none-eabi-gcc` 的 `@响应文件` 被中文路径损坏"的老坑
(本机 `LongPathsEnabled = 0`, 所以长度也是实打实的约束)。

### 换位置之后必做的两件事

⚠ **必须 `flutter clean`。** `build/windows/x64/CMakeCache.txt` 里写死了旧的绝对
路径, 不清会报
`The current CMakeCache.txt directory ... is different than the directory ... where CMakeCache.txt was created`。
`rust/target` 的指纹同理, 也要删。

⚠ **用 `Move-Item` 搬这个工程会搬到一半才失败。**
`windows/flutter/ephemeral/.plugin_symlinks/` 下是指向 `rust_builder` 的符号链接,
Move-Item 跟随失效链接时报 `Could not find a part of the path` —— 而此时大部分文件
已经搬过去了, 留下半个工程。先删掉 `build/`、`.dart_tool/`、`rust/target/`、
`windows/flutter/ephemeral/` 再搬, 它们都是构建期生成物, `flutter pub get` 会重建。

---

## ⚠ MSVC 与中文注释

`windows/CMakeLists.txt` 的 `APPLY_STANDARD_SETTINGS` 里加了 `/utf-8`。
不加的话 MSVC 会按系统代码页(中文 Windows = 936)解析 UTF-8 源码, 对
`windows/runner/*.cpp` 里的中文注释报 `C4819`, 而同一处的 `/WX` 把警告升成错误,
构建直接失败。

比给每个文件加 BOM 更稳: BOM 容易被编辑器或 git 配置吃掉, 新加的文件也容易忘。

---

## 竖窗尺寸

| 项 | 值 | 位置 |
|---|---|---|
| **期望**尺寸 | 480 × 960 逻辑像素 | `windows/runner/main.cpp` 的 `kDesired*Logical` |
| 实际尺寸 | 按显示器工作区**夹取** | `main.cpp` 的 `ClampToWorkArea()` |
| 最小尺寸 | 360 × 560 逻辑像素 | `win32_window.cpp` 的 `WM_GETMINMAXINFO` |

全部是**逻辑像素**, Win32 侧会按窗口所在显示器的 DPI 放大。

⚠ **为什么必须夹取(实测教训)**: 不夹取的话小屏或高缩放比下窗口会超出屏幕,
而且**没有任何报错**。本机实测 —— 主屏 1280×720、工作区仅 **672px 高**,
960 逻辑像素的窗口被系统截断成 737 物理像素, 底部内容直接看不见。
夹取后实测 480 × 632 物理像素, 放得进工作区。

夹取而不是把常量调小: 调小会让真正的 1080p+ 屏白白浪费高度。期望值保持"理想
尺寸", 由运行时按实际屏幕退让。`ClampToWorkArea` 与 `Win32Window::Create` 用的是
同一个显示器(`MonitorFromPoint(origin)`)和同一个 DPI 来源
(`FlutterDesktopGetDpiForMonitor`) —— 这两处必须一致, 否则夹取用的比例和实际放大
用的比例不同, 白算。

窗口标题保持 ASCII (`sensor_beacon tool`), 免得标题栏乱码难查;
要中文标题用 Dart 侧的 `window_manager` 包在运行时设。

---

## ⚠ 这个设备的硬约束(会打乱原生 app 的常规设计)

写 UI 之前必须知道。前三条来自固件:

1. **不能做自动重连。** 可连接广播只有 30 秒窗口
   (`BLE_LINK_ADV_DURATION_MS`), 而且**只能靠人物理长按按键**打开。
   "记住设备 → 后台重连 → 掉线自动恢复"这套标准套路**全部失效** ——
   不要写重连循环, 它永远等不到设备。UI 要围绕"请去按住按键"这个动作设计,
   并给窗口倒计时。

2. **扫描与连接互斥。** S112 只有 1 个广播集 + 1 个连接槽, 连接期间设备
   **不播数据广播**。`rust/src/ble` 用状态机强制串行化, 别在 Dart 侧绕过它。

3. **设备平时完全静默。** 扫描列表长期空白是**正常状态**, 不是故障。
   界面上那段提示文案不是客套话 —— 删掉它, 第一次用的人一定会以为工具坏了。
   `test/widget_test.dart` 有一条测试专门钉住这段文案。

后两条来自实测:

4. **Windows 收包率天然很低。** 系统的 BLE 侦听器约每 1.3 秒才开一次扫描窗口
   (`dev/desktop/scanner` 的 `dupchk` 实测: 到达间隔中位 1346ms), 而设备一轮
   burst 只持续 1.5 秒 —— 45 个空中包能采到一两个就是正常结果。
   Windows 不暴露 scan window/interval 设置, **应用层无法修复, 换框架也一样**。
   → 所以定位是: **广播扫描只当"活体检测/快速一瞥", 真正的数据采集走连接后的
   `REC_READ`**(逐字节可靠, 有 ATT 确认与重传)。

5. **拿不到协商后的 MTU。** btleplug 不暴露, WinRT 自己协商。所以命令协议
   (note 06)把帧长钉死 20 字节的决定在原生栈下同样成立 —— 不要因为"上原生了"
   去改协议。

---

## 现状与下一步

**已完成**(骨架验证: `cargo test` 22 passed / `flutter analyze` 0 issues /
Windows debug 构建通过, `sensor_tool_core.dll` 随 exe 打包):

- FRB 桥打通, Rust 侧自持 tokio runtime(FRB 的执行器不是 tokio, 混用会 panic)
- 广播协议 v0x02 解析 + 单测(24bit 符号扩展边界、v3 明确拒绝而非误解、
  别人的 0xFFFF 包被正确过滤)
- 命令协议 v1 帧层 + 多帧重组 + 单测(丢帧检测、回显误判、帧交织、字节预算自洽)
- 适配器枚举、扫描、按 (device_id, counter) 的轮次去重
- 竖窗 + 最小尺寸约束

**尚未做**:

- **GATT 连接**(命令协议的传输层) —— 整条路上最大的未验证点。
  `dev/desktop/scanner` 只验证过扫描, **连接一次都没跑过**。
- 各命令的载荷结构解析(`proto/payload.rs`) —— 等固件实现出来才好实测
- 串口通路(`serialport` crate) —— 别忘了它是唯一能长时间连续观测的通路
- 最小二乘标定向导、记录下载与 CSV、配置面板

**固件侧的硬前置**: note 06 §12 的清单里至少要先有 `CMD_INFO` 与
`CMD_REC_READ`, 连接层才有东西可测。当前固件的 `nus_data_handler` 只做
hexdump + 原样回显 —— `proto/cmd.rs` 里有一条测试
(`rejects_echo_from_current_firmware`)专门覆盖这个现状,
免得把回显误当响应解。
