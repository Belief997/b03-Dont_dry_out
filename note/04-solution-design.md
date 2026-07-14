# 04 · 方案设计与讨论

## 1. 需求回顾

| 项 | 结论 |
|----|------|
| 芯片 | nRF52810 + S112 |
| 常态 | 深度睡眠(极低功耗) |
| 唤醒源 | 外部比较器输出:**事件发生 → 上升沿 → 保持高**;条件消失后**自动回落到低** |
| 唤醒后动作 | **等待 1~2s 传感器数据稳定 → 采集 → 广播上报 → 回睡** |
| 触发频率 | 稀疏(分钟级或更长) |
| 采集 | 传感器模拟量(SAADC)+ 电池电压(内部 VDD) |
| 广播 | **非连接广播**,包含**设备 ID + 8bit 计数器 + 采样值** |
| 供电 | CR2032 纽扣电池;传感器常供电 |
| 时钟 | 板上有 32.768kHz 晶振 |

## 2. 深睡策略:System OFF vs System ON

| | System OFF | System ON (idle) |
|---|---|---|
| 电流 | ~0.4µA | ~1.5µA |
| 唤醒方式 | **复位**(从头执行) | 断点续跑(RAM/寄存器保留) |
| RAM 保持 | 否(仅 GPREGRET 等保持寄存器) | 是 |
| GPIO 唤醒 | **SENSE/DETECT(电平)** | GPIOTE(真边沿) |

**决策:采用 System OFF + 复位唤醒。** 理由:事件稀疏 + 纽扣电池,静态功耗压倒一切;跨睡眠只需保留极小状态(计数器),用保持寄存器即可,不必保 RAM。

## 3. 跨睡眠状态:GPREGRET

- System OFF 会掉 RAM,但 **GPREGRET / GPREGRET2**(各 8bit)属常开电源域,**跨 System OFF 与复位保持**,仅上电/掉电复位清零。
- 8bit 事件计数器正好放 `GPREGRET`。
- **访问方式二选一(取决于 SoftDevice 是否已使能)**:
  - SD 未使能:直接寄存器 `NRF_POWER->GPREGRET`(可整字节写)。
  - SD 已使能:必须用 `sd_power_gpregret_set/clr/get`(SVC)。
- 冷启动判定:读 `RESETREAS`;`==0` 为上电复位 → 计数器归零;`bit16 (OFF)` 置位 → 由 System OFF 的 SENSE 唤醒。读后写 1 清 `RESETREAS`。

## 4. 核心难点:上升沿保持 + System OFF 的"立即重唤醒"

外部比较器事件时**上升沿后持续保持高电平**。而 System OFF 的唤醒是**电平检测(SENSE)**,不是边沿:
- 若唤醒后仍武装"高电平唤醒",则处理完回到 System OFF 时,引脚**还在高** → **立刻又被唤醒**,陷入死循环。

### 解法:SENSE 翻转状态机(用引脚电平自身编码状态)

不需要额外"状态标志",唤醒后读一次引脚电平即可判断处于哪个阶段:

```
          [待事件] 武装 SENSE = HIGH
               │  比较器上升沿(高电平)
               ▼
   唤醒(复位)→ 读引脚 = 高 ── 事件段 ──► 计数+1 → 等稳定 → 采集 → 广播
               │                                        │
               │                          武装 SENSE = LOW(等信号回落)
               │                                        ▼
               │                                   System OFF
               │
   唤醒(复位)→ 读引脚 = 低 ── 清除段 ──► 不广播 → 武装 SENSE = HIGH → System OFF
                                                        │
                                                   回到 [待事件]
```

- **每个物理事件恰好 2 次唤醒**:一次高(采集+广播),一次低(信号回落时重新武装)。
- 高电平段结束时武装 `SENSE=LOW`:因为此刻引脚是高,不会误触发;待信号回落到低时才唤醒。
- 低电平段(或冷启动)武装 `SENSE=HIGH`:回到等待下一次事件。

## 5. 唤醒后时序(重要:先等待,再采集)

> ⚠ 关键澄清:**1~2s 是"传感器上电稳定"的等待,发生在采集之前**;不是广播时长。

```
唤醒(复位)
  → log/读 RESETREAS + GPREGRET(SD 未使能,直接寄存器)
  → 读唤醒引脚电平
  ── 高电平(事件)──►
       计数器 +1,写回 GPREGRET(直接)
       使能 S112(启动 LFCLK=XTAL)
       init: app_timer / pwr_mgmt / SAADC
       ┌─ 等待 1~2s 传感器稳定 ─┐   ← app_timer(RTC)定时 + WFE 空闲
       └───────── 严禁 nrf_delay 忙等 ┘
       SAADC 采集:CH0 传感器(AINx) + CH1 电池(内部 VDD)
       组装负载 → 非连接广播 → 广播时长到(ADV_SET_TERMINATED)
       武装 SENSE=LOW → sd_power_system_off()
  ── 低电平(清除/冷启动)──►
       武装 SENSE=HIGH → System OFF(直接 SYSTEMOFF=1)
```

### 时序铁律
1. **稳定等待必须低功耗**:用 `app_timer`(基于 RTC/LFCLK)定时 + `nrf_pwr_mgmt_run()`(WFE)让 CPU 空闲;**绝不能** `nrf_delay_ms` 忙等(否则 1~2s CPU 满载,功耗白费)。
2. **LFCLK 依赖**:`app_timer` 需要 LFCLK 运行。本工程在事件段**先使能 S112**(SD 负责启动 XTAL),**再** `app_timer_init` + `start`。
3. **SoftDevice 访问边界**:使能 SD 之前用直接寄存器(RESETREAS/GPREGRET/SYSTEMOFF);使能之后一律用 `sd_power_*`。低电平段不使能 SD,全程直接寄存器,省时省电。

## 6. 广播负载(厂商自定义数据)

非连接广播类型:`BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED`。
负载放在 Manufacturer Specific Data:

| 字段 | 字节 | 来源 |
|------|------|------|
| Company ID | 2 | `0xFFFF`(SIG 保留/测试用,TODO 换正式分配值) |
| 魔数 | 1 | `0xAB`,供网关在通用 Company ID 下过滤他人广播 |
| 版本 | 1 | `0x01`,负载格式版本,供演进 |
| 设备 ID | 2(可 4) | `FICR->DEVICEID[0]` 低位 |
| 计数器 | 1 | GPREGRET 中的 8bit |
| 传感器采样 | 2 | SAADC CH0 raw(小端) |
| 电池 | 2 | SAADC CH1 → 毫伏(小端) |

31 字节广播空间充裕。**完整字节结构、字节序、网关解析示例(Python/C)与去重/版本策略见对接协议
[05-broadcast-payload-protocol.md](05-broadcast-payload-protocol.md)**(权威文档)。

## 7. 电池测量(无需外部引脚)

- SAADC 用**内部 VDD 输入** `NRF_SAADC_INPUT_VDD` 直接测电源电压,不占外部引脚、不需分压。
- 默认单端通道:增益 **1/6**、参考 **内部 0.6V** → 满量程 `0.6 / (1/6) = 3.6V`,恰好覆盖 CR2032(≈3.0V,新电池 3.3V)。
- 12bit:`mV = raw × 3600 / 4096`。
- 传感器通道同样用默认 SE 配置(满量程 3.6V);若传感器输出范围不同,调整增益/参考。

## 8. 功耗预算(量级)

| 阶段 | 电流 | 时长 |
|------|------|------|
| System OFF(待事件/待回落) | ~0.4µA | 绝大部分时间 |
| 稳定等待(WFE + SD idle,LFCLK) | ~数µA | 1~2s |
| SAADC 采集 | 数百µA~mA | ms 级 |
| BLE 广播(每次 TX 瞬时) | 数 mA 峰值 | 广播窗内间歇 |

事件稀疏(分钟级)下,平均电流主要由 System OFF 决定,CR2032 可支撑很长时间。若要进一步省电:缩短广播时长/降低 TX 功率/减少广播次数。

## 9. 待定项(TODO,已在 main.c 顶部集中标注)

| 参数 | 宏 | 说明 |
|------|----|----|
| 唤醒引脚 | `WAKE_PIN` | 比较器输出实际接的 GPIO(当前 13 = DK Button1 便于台架) |
| 引脚上下拉 | `WAKE_PIN_PULL` | 比较器推挽→NOPULL;开漏→上/下拉 |
| 传感器通道 | `SENSOR_AIN` | 实际模拟输入引脚 AINx |
| 稳定时间 | `SETTLING_TIME_MS` | 按传感器手册,1000~2000 |
| 广播间隔/时长/功率 | `ADV_INTERVAL_MS` / `ADV_DURATION_MS` / `ADV_TX_POWER_DBM` | 上报可靠性 vs 功耗 |
| 公司标识 | `APP_COMPANY_IDENTIFIER` | 有 SIG 分配则替换 0xFFFF |
| 设备 ID 长度 | `DEVICE_ID_LEN` | 2 或 4 字节 |

## 10. 待确认的硬件问题

- 比较器输出是**推挽**还是**开漏**?决定唤醒引脚上下拉配置。
- "条件消失自动回落到低"的时间尺度?若回落极快,低电平唤醒仍能捕获(SENSE 为电平锁存,回落沿会置 DETECT)。
- E73-TBA/TBB 模组实际是 52810 还是 52832?决定是否保留 `DEVELOP_IN_NRF52832`。
