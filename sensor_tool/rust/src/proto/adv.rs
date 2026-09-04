//! 广播协议解析 —— 对应 `dev/note/05-broadcast-payload-protocol.md`
//!
//! 这是**单向**通路: 设备单击按键时播 `BLE_BEACON_ADV_EVENTS` 个广播事件,
//! 内容放在厂商自定义段里。与连接后的命令协议(`super::cmd`)完全无关,
//! 两者各有独立版本号。
//!
//! ⚠ 权威来源是固件 `dev/app/sensor_beacon/main.c` 顶部的 `OFF_*` 宏与
//!   `payload_build()`, **不是** note 05 —— 那份文档写的还是 v0x01,
//!   而固件已经是 v0x02。本文件按 v0x02 实现。
//!
//! ⚠ 固件里还有一个已设计但**尚未接线**的 v0x03(`services/app_proto.h`),
//!   布局完全不同(8 条记录 × 2 字节, 播的是算好的克数而非原始计数)。
//!   `parse()` 按 version 字节分派, 遇到 0x03 返回 `UnsupportedVersion`
//!   而不是硬当 v2 解 —— 静默误读会画出垃圾曲线且不报错。

/// 厂商自定义段的 Company ID。0xFFFF 是 SIG 保留的测试 ID,
/// 与固件 `APP_COMPANY_IDENTIFIER` / `BLE_BEACON_COMPANY_ID` 一致。
///
/// ⚠ 0xFFFF 别人也可能在用, 所以过滤完 Company ID 还要校验魔数, 见 [`parse`]。
pub const COMPANY_ID: u16 = 0xFFFF;

/// 载荷首字节的魔数, 固件 `APP_PROTO_MAGIC`。
pub const MAGIC: u8 = 0xAB;

/// 本模块能解析的版本, 固件 `APP_PROTO_VERSION`。
pub const VERSION_V2: u8 = 0x02;

/// 已知但未实现的版本(固件 `app_proto.h` 里的 `APP_PROTO_VERSION_V3`)。
pub const VERSION_V3: u8 = 0x03;

/// v0x02 载荷长度, 固件 `MANUF_DATA_LEN` / `BLE_BEACON_PAYLOAD_LEN`。
pub const PAYLOAD_LEN_V2: usize = 16;

/// 传感器通道数。
pub const CH_COUNT: usize = 3;

// v0x02 字段偏移, 与 main.c 的 OFF_* 宏一一对应。
const OFF_MAGIC: usize = 0;
const OFF_VERSION: usize = 1;
const OFF_DEVICE_ID: usize = 2;
const DEVICE_ID_LEN: usize = 2;
const OFF_COUNTER: usize = OFF_DEVICE_ID + DEVICE_ID_LEN; // 4
const OFF_BATTERY: usize = OFF_COUNTER + 1; // 5
const OFF_SENSORS: usize = OFF_BATTERY + 2; // 7
const SENSOR_VAL_BYTES: usize = 3; // 有符号 24bit

/// 解析失败的原因。
///
/// 刻意分得细: 扫描环境里有上百个设备在播(实测 78 事件/秒、146 个设备),
/// 统计"因为什么被丢掉"才能判断是自己的设备漏收了, 还是别人的包被正确过滤了。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AdvParseError {
    /// 长度不符。别人的 0xFFFF 广播多半栽在这里。
    BadLength { got: usize, want: usize },
    /// 魔数不对 —— 确定不是本设备。
    BadMagic { got: u8 },
    /// 版本已知但本模块没实现(例如固件升到 v0x03 而工具没跟上)。
    UnsupportedVersion { got: u8 },
}

impl std::fmt::Display for AdvParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::BadLength { got, want } => {
                write!(f, "载荷长度 {got}, 期望 {want}")
            }
            Self::BadMagic { got } => write!(f, "魔数 0x{got:02X}, 期望 0x{MAGIC:02X}"),
            Self::UnsupportedVersion { got } => {
                write!(f, "不支持的协议版本 0x{got:02X}")
            }
        }
    }
}

impl std::error::Error for AdvParseError {}

/// 解析后的 v0x02 广播载荷。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AdvPayload {
    pub version: u8,
    /// 设备 ID, 取 `FICR->DEVICEID[0]` 的低 16 位。
    pub device_id: u16,
    /// 8bit 事件计数器, 一轮广播里的多个包**数值相同**, 客户端靠它去重。
    pub counter: u8,
    pub batt_mv: u16,
    /// 三通道原始计数, 已做 24bit 符号扩展。
    ///
    /// ⚠ 三通道增益不同(ch0=128, ch1=32, ch2=128), 载荷里**不携带增益字段**。
    ///   直接比较三者的原始计数是错的 —— ch1 与 ch0/ch2 有 4 倍灵敏度差。
    ///   实测灵敏度: ch0≈192.2, ch1≈49.3, ch2≈−199.9 counts/g(注意 ch2 为负)。
    pub ch: [i32; CH_COUNT],
}

/// 从厂商自定义段的载荷解析。
///
/// `payload` 是 **Company ID 之后**的字节 —— btleplug 的
/// `manufacturer_data: HashMap<u16, Vec<u8>>` 给的正是这一段, 不含 Company ID 本身。
pub fn parse(payload: &[u8]) -> Result<AdvPayload, AdvParseError> {
    // 先看长度够不够读魔数与版本, 再看魔数 —— 顺序反了会越界。
    if payload.len() < 2 {
        return Err(AdvParseError::BadLength {
            got: payload.len(),
            want: PAYLOAD_LEN_V2,
        });
    }

    if payload[OFF_MAGIC] != MAGIC {
        return Err(AdvParseError::BadMagic {
            got: payload[OFF_MAGIC],
        });
    }

    let version = payload[OFF_VERSION];
    if version != VERSION_V2 {
        // v3 也走这里 —— 宁可明确报"不支持", 也不要按 v2 布局硬解。
        return Err(AdvParseError::UnsupportedVersion { got: version });
    }

    if payload.len() != PAYLOAD_LEN_V2 {
        return Err(AdvParseError::BadLength {
            got: payload.len(),
            want: PAYLOAD_LEN_V2,
        });
    }

    let device_id = u16::from_le_bytes([payload[OFF_DEVICE_ID], payload[OFF_DEVICE_ID + 1]]);
    let batt_mv = u16::from_le_bytes([payload[OFF_BATTERY], payload[OFF_BATTERY + 1]]);

    let mut ch = [0i32; CH_COUNT];
    for (i, slot) in ch.iter_mut().enumerate() {
        let base = OFF_SENSORS + i * SENSOR_VAL_BYTES;
        *slot = read_s24_le(&payload[base..base + SENSOR_VAL_BYTES]);
    }

    Ok(AdvPayload {
        version,
        device_id,
        counter: payload[OFF_COUNTER],
        batt_mv,
        ch,
    })
}

/// 读一个小端有符号 24bit 值, 符号扩展到 i32。
///
/// ⚠ 必须做符号扩展: 实测读数达 ±8 万量级, 且 ch2 的灵敏度为负,
///   当零点在量程中间时负值是常态。当成无符号会把 −1 读成 16777215。
fn read_s24_le(b: &[u8]) -> i32 {
    debug_assert_eq!(b.len(), SENSOR_VAL_BYTES);
    let raw = (b[0] as u32) | ((b[1] as u32) << 8) | ((b[2] as u32) << 16);
    if raw & 0x0080_0000 != 0 {
        (raw | 0xFF00_0000) as i32
    } else {
        raw as i32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 构造一个合法的 v0x02 载荷。
    fn make(device_id: u16, counter: u8, batt: u16, ch: [i32; 3]) -> Vec<u8> {
        let mut p = vec![0u8; PAYLOAD_LEN_V2];
        p[OFF_MAGIC] = MAGIC;
        p[OFF_VERSION] = VERSION_V2;
        p[OFF_DEVICE_ID..OFF_DEVICE_ID + 2].copy_from_slice(&device_id.to_le_bytes());
        p[OFF_COUNTER] = counter;
        p[OFF_BATTERY..OFF_BATTERY + 2].copy_from_slice(&batt.to_le_bytes());
        for (i, v) in ch.iter().enumerate() {
            let base = OFF_SENSORS + i * SENSOR_VAL_BYTES;
            let u = *v as u32;
            p[base] = (u & 0xFF) as u8;
            p[base + 1] = ((u >> 8) & 0xFF) as u8;
            p[base + 2] = ((u >> 16) & 0xFF) as u8;
        }
        p
    }

    #[test]
    fn roundtrip_positive() {
        let p = make(0x1234, 7, 3010, [37836, 9960, 40395]);
        let got = parse(&p).unwrap();
        assert_eq!(got.device_id, 0x1234);
        assert_eq!(got.counter, 7);
        assert_eq!(got.batt_mv, 3010);
        assert_eq!(got.ch, [37836, 9960, 40395]);
    }

    /// ch2 实测灵敏度为负, 负读数是常态 —— 这条防的是符号扩展写错。
    #[test]
    fn negative_channel() {
        let p = make(1, 0, 3000, [-40395, -1, -8_388_608]);
        let got = parse(&p).unwrap();
        assert_eq!(got.ch, [-40395, -1, -8_388_608]);
    }

    /// 24bit 的两个端点必须精确, 不能被截断成反号。
    #[test]
    fn s24_boundaries() {
        assert_eq!(read_s24_le(&[0xFF, 0xFF, 0x7F]), 8_388_607); // +max
        assert_eq!(read_s24_le(&[0x00, 0x00, 0x80]), -8_388_608); // -min
        assert_eq!(read_s24_le(&[0xFF, 0xFF, 0xFF]), -1);
        assert_eq!(read_s24_le(&[0x00, 0x00, 0x00]), 0);
    }

    #[test]
    fn rejects_foreign_advert() {
        // 别人的 0xFFFF 广播: 魔数不对
        assert_eq!(
            parse(&[0x01, 0x02, 0x03]),
            Err(AdvParseError::BadMagic { got: 0x01 })
        );
    }

    /// v3 必须明确拒绝, 不能按 v2 硬解 —— 否则会画出垃圾数据且不报错。
    #[test]
    fn rejects_v3_instead_of_misparsing() {
        let mut p = make(1, 0, 3000, [0, 0, 0]);
        p[OFF_VERSION] = VERSION_V3;
        assert_eq!(
            parse(&p),
            Err(AdvParseError::UnsupportedVersion { got: VERSION_V3 })
        );
    }

    #[test]
    fn rejects_bad_length() {
        let mut p = make(1, 0, 3000, [0, 0, 0]);
        p.push(0xAA);
        assert!(matches!(parse(&p), Err(AdvParseError::BadLength { .. })));

        // 太短到读不出魔数
        assert!(matches!(parse(&[0xAB]), Err(AdvParseError::BadLength { .. })));
        assert!(matches!(parse(&[]), Err(AdvParseError::BadLength { .. })));
    }
}
