//! 连接态命令协议 v1 的编解码 —— 对应 `dev/note/06-ble-command-protocol.md`
//!
//! 这是**双向**通路: 长按按键开 30s 可连接窗口 → 连上 → 经 NUS 收发请求/响应。
//! 与广播协议(`super::adv`)完全无关, 两者各有独立版本号。
//!
//! ⚠ 固件侧**尚未实现任何一条命令**(`nus_data_handler` 目前只做 hexdump + 回显)。
//!   本模块按 note 06 的规范实现, 是"先有客户端、后有固件"的顺序。
//!   note 06 §12 是固件待实现清单。
//!
//! 本模块只管**字节层**: 组请求帧、解响应帧、多帧重组。
//! "什么时候发哪条命令"是上层(`crate::ble`)的事; 各命令的载荷结构解析放
//! `super::payload`(待建), 因为那部分依赖固件先实现出来才能实测。

/// 协议版本, note 06 的 `CMD_PROTO_VERSION`。
pub const PROTO_VERSION: u8 = 1;

/// 帧长上限。**v1 固定 20 字节, 不协商** —— 见 note 06 §1.2:
/// Web Bluetooth 不暴露协商后的 MTU, btleplug 也不暴露, 所以只能按最保守的
/// ATT MTU 23 算(23 − 3 = 20)。原生栈同样拿不到 MTU, 换技术栈也改不了这一条。
pub const MAX_FRAME: usize = 20;

/// 请求帧头长度: CMD + TAG。
pub const REQ_HDR_LEN: usize = 2;

/// 响应帧头长度: CMD|0x80 + TAG + STATUS + FRAG。
pub const RESP_HDR_LEN: usize = 4;

/// 单个请求帧最多带多少参数字节。
pub const ARGS_MAX: usize = MAX_FRAME - REQ_HDR_LEN; // 18

/// 单个响应帧最多带多少数据字节。
pub const DATA_MAX: usize = MAX_FRAME - RESP_HDR_LEN; // 16

/// 一个响应最多几帧(FRAG 的帧号占 7 位)。
pub const FRAMES_MAX: usize = 128;

/// 一个响应的数据上限。超过就必须由命令自己分页(`REC_READ` 的 `count ≤ 64` 即由此而来)。
pub const RESP_DATA_MAX: usize = FRAMES_MAX * DATA_MAX; // 2048

/// 响应帧的方向位。
const RESP_BIT: u8 = 0x80;

/// FRAG 字节的"还有后续帧"位。
const FRAG_MORE: u8 = 0x80;

/// FRAG 字节里帧号的掩码。
const FRAG_SEQ_MASK: u8 = 0x7F;

/// 设备主动上报所用的 TAG。客户端**不得**使用这个值。
pub const TAG_DEVICE_INITIATED: u8 = 0xFF;

/// 命令码。数值必须与固件 `cmd_proto.h`(待建)一致。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum Cmd {
    Info = 0x01,
    Sensor = 0x02,
    SensorStream = 0x03,
    CalGet = 0x10,
    CalEnter = 0x11,
    CalExit = 0x12,
    CalZeroSet = 0x13,
    CalKSet = 0x14,
    CfgSet = 0x20,
    CfgSave = 0x2F,
    RecRead = 0x30,
    RecErase = 0x31,
}

impl Cmd {
    pub fn from_u8(v: u8) -> Option<Self> {
        Some(match v {
            0x01 => Self::Info,
            0x02 => Self::Sensor,
            0x03 => Self::SensorStream,
            0x10 => Self::CalGet,
            0x11 => Self::CalEnter,
            0x12 => Self::CalExit,
            0x13 => Self::CalZeroSet,
            0x14 => Self::CalKSet,
            0x20 => Self::CfgSet,
            0x2F => Self::CfgSave,
            0x30 => Self::RecRead,
            0x31 => Self::RecErase,
            _ => return None,
        })
    }
}

/// `REC_ERASE` 的确认码, note 06 里的 `'ERAS'` 小端。
///
/// ⚠ 这是唯一会不可逆销毁用户数据的命令, 而通道无鉴权无加密。
///   确认码把误触发概率压到 2⁻³²。
pub const REC_ERASE_CONFIRM: u32 = 0x5341_5245;

/// 设备返回的状态码。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    Ok,
    UnknownCmd,
    BadLength,
    BadParam,
    WrongMode,
    Busy,
    Storage,
    NotFound,
    NotCalibrated,
    Confirm,
    Internal,
    /// 协议里没定义的值 —— 固件比客户端新, 或者固件有 bug。
    Unknown(u8),
}

impl Status {
    pub fn from_u8(v: u8) -> Self {
        match v {
            0x00 => Self::Ok,
            0x01 => Self::UnknownCmd,
            0x02 => Self::BadLength,
            0x03 => Self::BadParam,
            0x04 => Self::WrongMode,
            0x05 => Self::Busy,
            0x06 => Self::Storage,
            0x07 => Self::NotFound,
            0x08 => Self::NotCalibrated,
            0x09 => Self::Confirm,
            0xFF => Self::Internal,
            other => Self::Unknown(other),
        }
    }

    pub fn is_ok(self) -> bool {
        self == Self::Ok
    }
}

impl std::fmt::Display for Status {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            Self::Ok => "成功",
            Self::UnknownCmd => "命令码未定义",
            Self::BadLength => "请求帧长度不符",
            Self::BadParam => "参数越界",
            Self::WrongMode => "标定模式状态不符",
            Self::Busy => "设备忙(上一条未发完或 flash 操作中)",
            Self::NotFound => "记录索引越界",
            Self::Storage => "flash 读写失败",
            Self::NotCalibrated => "设备未标定",
            Self::Confirm => "确认码不对",
            Self::Internal => "设备内部错误",
            Self::Unknown(_) => "未知状态码",
        };
        match self {
            Self::Unknown(v) => write!(f, "{s}(0x{v:02X})"),
            _ => write!(f, "{s}"),
        }
    }
}

/// 组一个请求帧。
///
/// 返回的字节必须**一次 write 整帧发出, 不得分片** —— note 06 §1.3:
/// 协议不带 magic/长度字段, 靠 GATT 的写边界定帧。客户端自行分片会让固件误解析。
pub fn encode_request(cmd: Cmd, tag: u8, args: &[u8]) -> Result<Vec<u8>, CodecError> {
    if args.len() > ARGS_MAX {
        return Err(CodecError::ArgsTooLong {
            got: args.len(),
            max: ARGS_MAX,
        });
    }
    if tag == TAG_DEVICE_INITIATED {
        // 0xFF 是设备主动上报的保留值; 客户端用了它就无法把响应与请求对上。
        return Err(CodecError::ReservedTag);
    }

    let mut f = Vec::with_capacity(REQ_HDR_LEN + args.len());
    f.push(cmd as u8);
    f.push(tag);
    f.extend_from_slice(args);
    Ok(f)
}

/// 一个已解析的响应帧(还没重组)。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResponseFrame {
    /// 已去掉方向位的命令码原始值(可能不是已知的 `Cmd`)。
    pub cmd_raw: u8,
    pub tag: u8,
    pub status: Status,
    pub seq: u8,
    pub more: bool,
    pub data: Vec<u8>,
}

impl ResponseFrame {
    /// 这一帧是设备主动上报的吗(推流 / 标定超时通知)?
    pub fn is_device_initiated(&self) -> bool {
        self.tag == TAG_DEVICE_INITIATED
    }

    pub fn cmd(&self) -> Option<Cmd> {
        Cmd::from_u8(self.cmd_raw)
    }
}

/// 解一个响应帧。
pub fn decode_response(buf: &[u8]) -> Result<ResponseFrame, CodecError> {
    if buf.len() < RESP_HDR_LEN {
        return Err(CodecError::FrameTooShort { got: buf.len() });
    }
    if buf.len() > MAX_FRAME {
        return Err(CodecError::FrameTooLong { got: buf.len() });
    }

    let cmd_byte = buf[0];
    if cmd_byte & RESP_BIT == 0 {
        // 收到一个没置方向位的帧 —— 要么固件把请求回显了(当前固件正是这样!),
        // 要么协议对不上。明确报错, 别当响应解。
        return Err(CodecError::NotAResponse { got: cmd_byte });
    }

    let frag = buf[3];
    Ok(ResponseFrame {
        cmd_raw: cmd_byte & !RESP_BIT,
        tag: buf[1],
        status: Status::from_u8(buf[2]),
        seq: frag & FRAG_SEQ_MASK,
        more: frag & FRAG_MORE != 0,
        data: buf[RESP_HDR_LEN..].to_vec(),
    })
}

/// 多帧响应重组器。
///
/// note 06 §2.3: 帧只是搬运工, 各帧 `data` 按帧号拼成一段连续字节流, 再由
/// 上层按命令结构解析。除末帧外每帧 `data` 必须**满 16 字节** —— 客户端据此
/// 校验没丢帧(BLE 通知在发送队列溢出时会丢, 固件侧要靠 TX_RDY 续发)。
#[derive(Debug, Default)]
pub struct Reassembler {
    cmd_raw: Option<u8>,
    tag: Option<u8>,
    status: Option<Status>,
    next_seq: u8,
    buf: Vec<u8>,
}

/// 重组器吃进一帧之后的结果。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Assembled {
    /// 还要更多帧。
    Pending,
    /// 拼完了。
    Complete {
        cmd_raw: u8,
        tag: u8,
        status: Status,
        data: Vec<u8>,
    },
}

impl Reassembler {
    pub fn new() -> Self {
        Self::default()
    }

    /// 丢掉当前进度(断连、超时、或换了一条命令时调用)。
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    pub fn is_idle(&self) -> bool {
        self.cmd_raw.is_none()
    }

    /// 吃进一帧。
    ///
    /// 出错时重组器会**自动 reset** —— 一个坏帧之后继续往旧缓冲里拼只会产出
    /// 更难诊断的垃圾, 不如立刻放弃这一轮让上层重发。
    pub fn push(&mut self, f: ResponseFrame) -> Result<Assembled, CodecError> {
        let r = self.push_inner(&f);
        if r.is_err() {
            self.reset();
        }
        r
    }

    fn push_inner(&mut self, f: &ResponseFrame) -> Result<Assembled, CodecError> {
        // 失败响应永远是单帧(note 06 §2.2: STATUS != 0 时 FRAG=0、DATA 空),
        // 直接短路 —— 不要求它满足下面的满帧规则。
        if !f.status.is_ok() && self.is_idle() {
            return Ok(Assembled::Complete {
                cmd_raw: f.cmd_raw,
                tag: f.tag,
                status: f.status,
                data: Vec::new(),
            });
        }

        match self.cmd_raw {
            None => {
                if f.seq != 0 {
                    // 第一帧的帧号必须是 0。不是 0 说明前面的帧丢了。
                    return Err(CodecError::UnexpectedSeq {
                        want: 0,
                        got: f.seq,
                    });
                }
                self.cmd_raw = Some(f.cmd_raw);
                self.tag = Some(f.tag);
                self.status = Some(f.status);
            }
            Some(cmd_raw) => {
                // 同一轮的 cmd/tag 必须一致, 否则是两条命令的帧交织了。
                if cmd_raw != f.cmd_raw || self.tag != Some(f.tag) {
                    return Err(CodecError::FrameMismatch);
                }
                if f.seq != self.next_seq {
                    return Err(CodecError::UnexpectedSeq {
                        want: self.next_seq,
                        got: f.seq,
                    });
                }
            }
        }

        // 非末帧必须满 16 字节 —— 这是丢帧检测的关键。
        if f.more && f.data.len() != DATA_MAX {
            return Err(CodecError::ShortNonFinalFrame { got: f.data.len() });
        }

        if self.buf.len() + f.data.len() > RESP_DATA_MAX {
            return Err(CodecError::ResponseTooLong);
        }

        self.buf.extend_from_slice(&f.data);

        if f.more {
            self.next_seq = self
                .next_seq
                .checked_add(1)
                .filter(|s| (*s as usize) < FRAMES_MAX)
                .ok_or(CodecError::TooManyFrames)?;
            Ok(Assembled::Pending)
        } else {
            let out = Assembled::Complete {
                cmd_raw: f.cmd_raw,
                tag: f.tag,
                status: f.status,
                data: std::mem::take(&mut self.buf),
            };
            self.reset();
            Ok(out)
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CodecError {
    ArgsTooLong { got: usize, max: usize },
    ReservedTag,
    FrameTooShort { got: usize },
    FrameTooLong { got: usize },
    NotAResponse { got: u8 },
    UnexpectedSeq { want: u8, got: u8 },
    ShortNonFinalFrame { got: usize },
    FrameMismatch,
    ResponseTooLong,
    TooManyFrames,
}

impl std::fmt::Display for CodecError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ArgsTooLong { got, max } => write!(f, "参数 {got} 字节, 上限 {max}"),
            Self::ReservedTag => write!(f, "TAG 0xFF 是设备主动上报的保留值"),
            Self::FrameTooShort { got } => {
                write!(f, "响应帧只有 {got} 字节, 至少需要 {RESP_HDR_LEN}")
            }
            Self::FrameTooLong { got } => write!(f, "响应帧 {got} 字节, 上限 {MAX_FRAME}"),
            Self::NotAResponse { got } => {
                write!(f, "首字节 0x{got:02X} 未置响应位(固件可能仍是回显实现)")
            }
            Self::UnexpectedSeq { want, got } => {
                write!(f, "帧号错乱: 期望 {want}, 收到 {got}(可能丢帧)")
            }
            Self::ShortNonFinalFrame { got } => {
                write!(f, "非末帧只有 {got} 字节数据, 应满 {DATA_MAX}(可能丢帧)")
            }
            Self::FrameMismatch => write!(f, "帧的 cmd/tag 与当前重组轮次不符"),
            Self::ResponseTooLong => write!(f, "响应超过 {RESP_DATA_MAX} 字节上限"),
            Self::TooManyFrames => write!(f, "响应超过 {FRAMES_MAX} 帧上限"),
        }
    }
}

impl std::error::Error for CodecError {}

#[cfg(test)]
mod tests {
    use super::*;

    fn resp(cmd: Cmd, tag: u8, status: u8, seq: u8, more: bool, data: &[u8]) -> Vec<u8> {
        let mut f = vec![
            cmd as u8 | RESP_BIT,
            tag,
            status,
            seq | if more { FRAG_MORE } else { 0 },
        ];
        f.extend_from_slice(data);
        f
    }

    #[test]
    fn request_roundtrip() {
        let f = encode_request(Cmd::RecRead, 0x07, &[0x00, 0x00, 40]).unwrap();
        assert_eq!(f, vec![0x30, 0x07, 0x00, 0x00, 40]);
        assert!(f.len() <= MAX_FRAME);
    }

    #[test]
    fn request_rejects_reserved_tag() {
        assert_eq!(
            encode_request(Cmd::Info, TAG_DEVICE_INITIATED, &[]),
            Err(CodecError::ReservedTag)
        );
    }

    #[test]
    fn request_rejects_overlong_args() {
        let args = vec![0u8; ARGS_MAX + 1];
        assert!(matches!(
            encode_request(Cmd::CfgSet, 1, &args),
            Err(CodecError::ArgsTooLong { .. })
        ));
    }

    #[test]
    fn single_frame_response() {
        let raw = resp(Cmd::Sensor, 3, 0, 0, false, &[1, 2, 3]);
        let f = decode_response(&raw).unwrap();
        assert_eq!(f.cmd(), Some(Cmd::Sensor));
        assert_eq!(f.tag, 3);
        assert!(f.status.is_ok());
        assert!(!f.more);

        let mut ra = Reassembler::new();
        assert_eq!(
            ra.push(f).unwrap(),
            Assembled::Complete {
                cmd_raw: Cmd::Sensor as u8,
                tag: 3,
                status: Status::Ok,
                data: vec![1, 2, 3],
            }
        );
        assert!(ra.is_idle());
    }

    /// INFO 是 40 字节 → 3 帧(16+16+8)。这是最典型的多帧场景。
    #[test]
    fn multi_frame_info() {
        let payload: Vec<u8> = (0u8..40).collect();
        let mut ra = Reassembler::new();
        let mut out = None;

        for (i, chunk) in payload.chunks(DATA_MAX).enumerate() {
            let more = (i + 1) * DATA_MAX < payload.len();
            let raw = resp(Cmd::Info, 9, 0, i as u8, more, chunk);
            let f = decode_response(&raw).unwrap();
            match ra.push(f).unwrap() {
                Assembled::Pending => assert!(more),
                Assembled::Complete { data, .. } => out = Some(data),
            }
        }
        assert_eq!(out.unwrap(), payload);
    }

    /// 丢中间一帧必须被抓到, 不能静默拼出一段短了 16 字节的数据。
    #[test]
    fn detects_dropped_frame() {
        let mut ra = Reassembler::new();
        let f0 = decode_response(&resp(Cmd::Info, 1, 0, 0, true, &[0u8; 16])).unwrap();
        assert_eq!(ra.push(f0).unwrap(), Assembled::Pending);

        // 跳过 seq=1, 直接来 seq=2
        let f2 = decode_response(&resp(Cmd::Info, 1, 0, 2, false, &[9u8; 4])).unwrap();
        assert_eq!(
            ra.push(f2),
            Err(CodecError::UnexpectedSeq { want: 1, got: 2 })
        );
        // 出错后必须自动复位, 否则下一轮会接着旧缓冲拼
        assert!(ra.is_idle());
    }

    /// 非末帧不满 16 字节 —— 另一种丢帧的表现。
    #[test]
    fn detects_short_non_final_frame() {
        let mut ra = Reassembler::new();
        let f = decode_response(&resp(Cmd::Info, 1, 0, 0, true, &[1, 2, 3])).unwrap();
        assert_eq!(ra.push(f), Err(CodecError::ShortNonFinalFrame { got: 3 }));
    }

    #[test]
    fn error_response_is_single_frame() {
        let raw = resp(Cmd::RecErase, 5, 0x09, 0, false, &[]);
        let f = decode_response(&raw).unwrap();
        assert_eq!(f.status, Status::Confirm);

        let mut ra = Reassembler::new();
        match ra.push(f).unwrap() {
            Assembled::Complete { status, data, .. } => {
                assert_eq!(status, Status::Confirm);
                assert!(data.is_empty());
            }
            Assembled::Pending => panic!("失败响应必须是单帧"),
        }
    }

    /// 当前固件的 nus_data_handler 是**原样回显**。客户端连上去发 INFO 会收到
    /// 自己发的 [0x01, tag] —— 首字节没有响应位。必须报错而不是误当响应。
    #[test]
    fn rejects_echo_from_current_firmware() {
        let echoed = vec![Cmd::Info as u8, 0x01];
        // 回显只有 2 字节, 先栽在长度上
        assert!(matches!(
            decode_response(&echoed),
            Err(CodecError::FrameTooShort { .. })
        ));

        // 就算长度够了, 也要因为没置方向位而被拒
        let echoed_long = vec![Cmd::CalKSet as u8, 0x01, 0x00, 0x00, 0xFF];
        assert_eq!(
            decode_response(&echoed_long),
            Err(CodecError::NotAResponse {
                got: Cmd::CalKSet as u8
            })
        );
    }

    #[test]
    fn rejects_oversized_frame() {
        let raw = vec![0x81, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(raw.len(), MAX_FRAME + 1);
        assert!(matches!(
            decode_response(&raw),
            Err(CodecError::FrameTooLong { .. })
        ));
    }

    /// 两条命令的帧交织(设备主动推流插进多帧响应中间)必须被抓到。
    #[test]
    fn detects_interleaved_frames() {
        let mut ra = Reassembler::new();
        let f0 = decode_response(&resp(Cmd::Info, 1, 0, 0, true, &[0u8; 16])).unwrap();
        assert_eq!(ra.push(f0).unwrap(), Assembled::Pending);

        // 推流帧(不同 cmd + TAG=0xFF)混进来
        let stream = decode_response(&resp(
            Cmd::Sensor,
            TAG_DEVICE_INITIATED,
            0,
            1,
            false,
            &[7u8; 4],
        ))
        .unwrap();
        assert_eq!(ra.push(stream), Err(CodecError::FrameMismatch));
    }

    /// 上层必须能靠 TAG 认出主动上报, 不能把它当成上一条请求的响应。
    #[test]
    fn device_initiated_is_recognizable() {
        let raw = resp(Cmd::Sensor, TAG_DEVICE_INITIATED, 0, 0, false, &[1, 2]);
        let f = decode_response(&raw).unwrap();
        assert!(f.is_device_initiated());
    }

    /// 帧长上限的算术必须自洽 —— 改任何一个常量都不能破坏这组关系。
    #[test]
    fn frame_budget_is_consistent() {
        assert_eq!(REQ_HDR_LEN + ARGS_MAX, MAX_FRAME);
        assert_eq!(RESP_HDR_LEN + DATA_MAX, MAX_FRAME);
        assert_eq!(RESP_DATA_MAX, FRAMES_MAX * DATA_MAX);
        // REC_READ 一次最多 64 条 × 20 字节 + 1 字节条数, 必须放得进上限
        assert!(1 + 64 * 20 <= RESP_DATA_MAX);
    }
}
