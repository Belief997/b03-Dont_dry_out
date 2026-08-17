# 广播仲裁 —— 未完成，未接入工程

**状态：设计完成，代码未编译验证，不在 .uvprojx 里。**

2026-08-17 暂停。原因：先验证"上电即可连接广播"这个已有功能在真机上工作正常，
再推进广播切换。放在 `services/` 之外就是为了避免被误当作已就绪代码。

## 已确定的方案

平时播不可连接的传感器数据广播；打开可连接广播期间**和**连接期间，数据广播都停止；
断开连接后恢复数据广播。

即广播集在任一时刻只归一个模块，交接点是"断开连接"。选这个而不是"连接期间继续播
数据"，是因为需求明确"平时的数据广播不许被连接"，且这样时序简单得多。

（技术上连接期间是**可以**继续播不可连接广播的 —— Broadcaster 与 Peripheral 是
两个独立 role。SDK 自带 eddystone 就这么做。但本工程不需要，故不采用。）

## 这两个模块解决的真实缺陷

S112 只有一个广播集（`components/softdevice/s112/headers/ble_gap.h:222`，
`BLE_GAP_ADV_SET_COUNT_MAX == 1`），而当前 `main.c:278` 与 `services/ble_link.c:67`
**各自持有一个 `m_adv_handle`**。谁第二个拿 `BLE_GAP_ADV_SET_HANDLE_NOT_SET` 去调
`sd_ble_gap_adv_set_configure()`，就会收到 `NRF_ERROR_NO_MEM`：

> Not enough memory to configure a new advertising handle.
> Update an existing advertising handle instead.

今天没暴露，只因为 `advertising_init()` 仅被 `#if 0` 死分支里的 `handle_event_active()`
调用。两条广播路径一旦都激活，这是必然的运行期失败。

`ble_adv_mux` 的作用就是独占持有那唯一的句柄，两个广播源都只提交"内容 + 参数"。
SDK 自带 eddystone 是同一思路（句柄由 `nrf_ble_es.c:59` 独占）。

## 文件

| 文件 | 作用 | 状态 |
|---|---|---|
| `ble_adv_mux.h/.c` | 广播集仲裁，独占 adv handle | 未编译验证 |
| `ble_beacon.h/.c` | 不可连接数据广播（双缓冲，可不中断更新） | 未编译验证 |

`ble_beacon` 的载荷格式与 `main.c` 现有 beacon 路径一致（`MANUF_DATA_LEN = 16`，
Company ID `0xFFFF`，无设备名），网关侧不需要改。

## 恢复实现时还差什么

1. **`ble_link.c` 的 `DISCONNECTED` 分支**：当前是"自动重开可连接广播"，
   按新方案应改成"通知上层，由 main.c 决定恢复数据广播"。需要加一个事件回调
   （如 `ble_link_evt_handler_t`，至少要能报 `DISCONNECTED` 和 `ADV_TIMEOUT`）。
   这一步曾改到一半，已回退 —— 现在 `ble_link.c` 是干净的原状态。

2. **`main.c` 改造**：把 `advertising_init()/advertising_start()/m_adv_handle`
   那套换成 `ble_beacon_*`；删掉 `main.c:278` 的 `m_adv_handle`；
   编排"谁在什么时候播"的策略（进入调试窗口的触发方式还没定 —— 按键长按？
   收到特定命令？开机固定窗口？这个需要产品侧决定）。

3. **`.uvprojx`**：4 个文件 × 2 个 target（Debug/Release）加入 Application 组，
   路径形如 `..\..\..\wip_adv_arbitration\ble_adv_mux.c`（若届时移回 `services/`
   则相应改路径）；`<IncludePath>` 视存放位置决定是否需要追加。

4. **可连接窗口超时**：建议用 `m_adv_params.duration` 让协议栈产生
   `ADV_SET_TERMINATED`（reason = `TIMEOUT` = 0x01），比自起 app_timer 省资源。
   上限 `BLE_GAP_ADV_TIMEOUT_LIMITED_MAX` = 18000（单位 10ms）= 180 秒。
   ⚠ 别照抄 eddystone 的 `APP_CFG_CONNECTABLE_ADV_TIMEOUT`：值 6000、注释写
   "in milliseconds"，但直接赋给 duration（单位 10ms），实际是 60 秒不是 6 秒。
