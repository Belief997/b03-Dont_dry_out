# 05 · 广播负载对接协议

供**网关 / 上位机 / 手机 App** 解析 sensor_beacon 广播包使用。
本文与固件 `dev/app/sensor_beacon/main.c` 的 `payload_build()` 及顶部宏**一一对应**;固件改布局时须**递增版本号**并同步本文。

> **协议版本:0x01**。负载最前含**魔数 `0xAB` + 版本 `0x01`**,用于在通用 Company ID 下过滤他人广播并支持格式演进。

---

## 1. 广播参数(扫描端须知)

| 项 | 值 | 对应宏 / 说明 |
|----|----|--------------|
| 广播类型 | 非连接非可扫描 (ADV_NONCONN_IND) | `BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED` |
| 是否有 Scan Response | **无** | 主动扫描也拿不到额外数据,被动扫描即可 |
| 广播间隔 | 默认 100 ms | `ADV_INTERVAL_MS` |
| 单次广播时长 | 默认 2000 ms | `ADV_DURATION_MS` → 每次事件约 **20 个广播事件**(3 个主信道各发) |
| PHY | 1M(传统广播) | S112 传统广播 |
| 发射功率 | 默认 0 dBm | `ADV_TX_POWER_DBM` |
| 设备 BLE 地址 | 随机静态地址(源自 FICR) | 扫描端也可见,可作设备身份备选,见 §5.1 |

**含义**:每发生一次事件,设备只在约 2s 内密集广播 ~20 份**相同**内容,然后深睡。扫描端应在这段窗口内抓到并**按"设备 ID + 计数器"去重**(见 §7)。

---

## 2. 完整广播包字节结构(空中 AD 数据)

`ble_advdata_encode()` 生成两个 AD 结构:Flags + Manufacturer Specific Data。

```
偏移  值        含义
────  ────────  ─────────────────────────────────────────────
[0]   0x02      AD#1 长度 (2)
[1]   0x01      AD 类型 = Flags
[2]   0x04      Flags 值 = BR/EDR Not Supported
────  ────────  ── 以上为 Flags,与业务无关,可跳过 ─────────────
[3]   0x0C      AD#2 长度 (12)
[4]   0xFF      AD 类型 = Manufacturer Specific Data
[5]   0xFF      Company ID 低字节  ┐ 小端 = 0xFFFF
[6]   0xFF      Company ID 高字节  ┘
────  ────────  ── 以下为 9 字节厂商自定义负载 ────────────────
[7]   0xAB      魔数 MAGIC
[8]   0x01      版本 VERSION
[9]   ID_lo     设备 ID 低字节   ┐ 16bit 小端
[10]  ID_hi     设备 ID 高字节   ┘
[11]  CNT       计数器 (8bit)
[12]  SEN_lo    传感器 raw 低字节 ┐ 16bit 小端
[13]  SEN_hi    传感器 raw 高字节 ┘
[14]  BAT_lo    电池 mV 低字节   ┐ 16bit 小端
[15]  BAT_hi    电池 mV 高字节   ┘
```

- 总长 16 字节(31 字节广播空间充裕)。
- **Company ID 与所有多字节字段一律小端 (little-endian)**;魔数/版本/计数器为单字节。
- 大多数 BLE 库(nRF Connect / bleak / BlueZ)会把 Manufacturer Specific Data 解析成 `{CompanyID: 负载字节}`,**负载字节就是上表 [7]~[15] 这 9 字节**(Company ID 已被库剥离并作为 key)。

---

## 3. 厂商负载字段定义(核心 9 字节)

以厂商负载起始为偏移 0(即上表 [7]):

| 偏移 | 长度 | 字段 | 类型/字节序 | 说明 | 固件宏 |
|:---:|:---:|------|------|------|------|
| 0 | 1 | 魔数 | uint8 | 固定 `0xAB`,过滤用 | `APP_PROTO_MAGIC` |
| 1 | 1 | 版本 | uint8 | 当前 `0x01`,决定后续解析 | `APP_PROTO_VERSION` |
| 2 | 2 | 设备 ID | uint16 LE | 源自 `FICR->DEVICEID[0]` 低 16 位 | `DEVICE_ID_LEN` |
| 4 | 1 | 计数器 | uint8 | 事件序号,0~255 循环 | 存于 GPREGRET |
| 5 | 2 | 传感器采样 | uint16 LE | **12bit 原始 ADC 值 (0~4095)** | `SAADC_CH_SENSOR` |
| 7 | 2 | 电池电压 | uint16 LE | **毫伏 (mV)**,已换算 | `SAADC_CH_BATTERY` |

**解析前置校验**:`负载[0]==0xAB && 负载[1]==0x01`,否则丢弃(非本协议 / 版本不符)。

> ⚠ 注意非对称:**传感器发的是原始 ADC 码**,**电池发的是已换算毫伏**。原因见 §5.3 / §5.4。

---

## 4. 完整报文示例

设备 ID=0x1234、计数器=5、传感器 raw=2048(0x0800)、电池=2950 mV(0x0B86):

**空中 16 字节(hex)**
```
02 01 04 0C FF FF FF AB 01 34 12 05 00 08 86 0B
                     └───────── 厂商负载 9B ─────────┘
```

**厂商负载(库解析出的 9 字节)**
```
AB 01 34 12 05 00 08 86 0B
```

**解码结果**
| 字段 | 原始字节 | 值 |
|------|---------|----|
| 魔数 | `AB` | 0xAB ✓ |
| 版本 | `01` | 1 ✓ |
| 设备 ID | `34 12` | 0x1234 = 4660 |
| 计数器 | `05` | 5 |
| 传感器 raw | `00 08` | 0x0800 = 2048 → 引脚电压 2048×3.6/4096 = **1.800 V** |
| 电池 | `86 0B` | 0x0B86 = 2950 → **2.950 V** |

---

## 5. 字段语义详解

### 5.1 设备 ID
- 取自芯片 `FICR->DEVICEID[0]` 的**低 `DEVICE_ID_LEN` 字节**(当前 2 字节 = 低 16 位)。
- ⚠ **16 位不保证全局唯一**:大规模部署可能碰撞。若需更强唯一性:
  - 把 `DEVICE_ID_LEN` 改为 4(用满 `DEVICEID[0]` 32 位),或
  - 直接用扫描到的**设备 BLE MAC 地址**(6 字节)作身份(Nordic 默认是源自 FICR 的随机静态地址,出厂稳定),或
  - 生产时在 UICR 写入自定义 ID。
- 改 `DEVICE_ID_LEN` 会使其后所有字段偏移平移,见 §9。

### 5.2 计数器
- 每次**真实事件**(高电平唤醒并广播)`+1`;`0~255` 溢出回绕。
- **上电冷启动(装电池 / 掉电)清零**(依据 RESETREAS 判定)。
- 用途:扫描端**去重**(同一事件的 ~20 份广播计数器相同)与**丢包检测**(相邻事件计数器应连续 +1;跳变说明漏收)。

### 5.3 传感器采样(原始码)
- 12bit SAADC 原始值 `0~4095`。
- **引脚电压** = `raw × 3.6 / 4096`(默认通道:增益 1/6、参考内部 0.6V → 满量程 3.6V)。
- 发原始码而非物理量,是因为**传感器的传递函数(电压↔物理量)属应用相关**,由上位机按具体传感器换算。若固件端已知换算关系,也可改为直接发物理量,须递增版本并同步本文。

### 5.4 电池电压
- 直接为**毫伏**。固件用内部 VDD 输入测量并换算:`mV = raw × 3600 / 4096`。
- 典型 CR2032:新电池约 3000~3300 mV,接近 2000 mV 视为欠压。阈值由上位机定。

---

## 6. 解析示例

### 6.1 Python(跨平台,`bleak`)
```python
import asyncio
from bleak import BleakScanner

COMPANY_ID = 0xFFFF
MAGIC      = 0xAB
VERSION    = 0x01

def parse_payload(mfg: bytes):
    if len(mfg) < 9:
        return None
    if mfg[0] != MAGIC or mfg[1] != VERSION:      # 前置校验:魔数 + 版本
        return None
    dev_id  = int.from_bytes(mfg[2:4], "little")
    counter = mfg[4]
    sensor  = int.from_bytes(mfg[5:7], "little")
    batt_mv = int.from_bytes(mfg[7:9], "little")
    return {
        "dev_id":     f"0x{dev_id:04X}",
        "counter":    counter,
        "sensor_raw": sensor,
        "sensor_v":   round(sensor * 3.6 / 4096, 3),
        "batt_mv":    batt_mv,
    }

def on_adv(device, adv):
    mfg = adv.manufacturer_data.get(COMPANY_ID)   # 库已剥离 Company ID
    if mfg is None:
        return
    rec = parse_payload(mfg)
    if rec:
        print(device.address, device.rssi, rec)

async def main():
    scanner = BleakScanner(detection_callback=on_adv)
    await scanner.start()
    await asyncio.sleep(30)
    await scanner.stop()

asyncio.run(main())
```

### 6.2 C(嵌入式网关 / nRF52 中心端 —— 手动遍历 AD)
```c
#define SB_MAGIC    0xAB
#define SB_VERSION  0x01

typedef struct {
    uint16_t dev_id;
    uint8_t  counter;
    uint16_t sensor_raw;
    uint16_t batt_mv;
} sensor_report_t;

/* data/len = 收到的 AD 负载。成功解析返回 true */
bool sensor_beacon_parse(const uint8_t *data, uint8_t len, sensor_report_t *out)
{
    uint8_t i = 0;
    while (i < len) {
        uint8_t fld_len = data[i];
        if (fld_len == 0 || i + 1 + fld_len > len) break;   /* 结束/畸形 */
        uint8_t        type    = data[i + 1];
        const uint8_t *val     = &data[i + 2];
        uint8_t        val_len = fld_len - 1;

        if (type == 0xFF && val_len >= 11) {                /* 厂商数据:company(2)+负载(9) */
            uint16_t company = val[0] | ((uint16_t)val[1] << 8);
            if (company == 0xFFFF) {
                const uint8_t *p = &val[2];                 /* 9 字节负载 */
                if (p[0] != SB_MAGIC || p[1] != SB_VERSION) /* 前置校验 */
                    return false;
                out->dev_id     = p[2] | ((uint16_t)p[3] << 8);
                out->counter    = p[4];
                out->sensor_raw = p[5] | ((uint16_t)p[6] << 8);
                out->batt_mv    = p[7] | ((uint16_t)p[8] << 8);
                return true;
            }
        }
        i += 1 + fld_len;
    }
    return false;
}
```

---

## 7. 去重与丢包处理

一次事件会收到多份(约 20)相同广播:

1. **去重键 = (设备 ID, 计数器)**:同键只处理一次(可加时间窗,如 5s 内同键忽略)。
2. **丢包检测**:同一设备相邻事件的计数器应 `+1`;若从 `n` 跳到 `n+k (k>1)`,说明漏收 `k-1` 次事件(注意 255→0 回绕)。
3. **首包/重启识别**:计数器回到 0 且之前非 0,通常表示设备重新上电。

---

## 8. 过滤与版本演进

- **过滤**:Company ID `0xFFFF` 是 SIG "保留/测试"值,任何测试设备都可能用同值。解析**必须**同时校验负载 `[0]=0xAB(魔数)`,才能可靠区分本产品广播。
- **版本**:`[1]=版本` 决定后续字段解析。日后若改负载布局:
  1. 固件递增 `APP_PROTO_VERSION`;
  2. 网关按版本分支解析(旧版本保留兼容);
  3. 同步本文并追加新版本的字段表。
- **进一步稳健化(可选)**:若要彻底避免与他人 `0xFFFF` 广播撞车,申请 Bluetooth SIG 正式 Company ID 替换 `APP_COMPANY_IDENTIFIER`(存储仍小端,如 Nordic 0x0059 → 字节 `59 00`)。

---

## 9. 布局变更影响对照

改动固件宏时,字段偏移与本文须同步,并**递增版本号**:

| 改动 | 影响 |
|------|------|
| `DEVICE_ID_LEN` 2→4 | 计数器/传感器/电池偏移各 **+2**;负载总长 9→11;AD#2 长度 0x0C→0x0E |
| 传感器改发物理量 | §3/§5.3 换算说明作废,改为约定单位/精度 |
| 新增字段 | 追加到负载末尾并更新 §2/§3(魔数/版本恒在最前) |
| 改 `APP_COMPANY_IDENTIFIER` | 更新 §1/§6 中的 Company ID 常量 |
| 改 `APP_PROTO_MAGIC` / `VERSION` | 更新 §2/§3/§6 常量;版本变更须网关分支兼容 |

> 权威来源始终是 `main.c` 的 `payload_build()` 与顶部 `APP_PROTO_*` / `OFF_*` / `*_LEN` 宏。
