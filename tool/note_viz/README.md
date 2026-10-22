# note_viz

把 `dev/note/06-ble-command-protocol.md` 变成可审阅的离线网页。**不改原文**，只做解析、结构化、交叉校验。

## 用法

```bash
cd dev/tool/note_viz
python build.py                 # 生成 out/06-ble-command-protocol.html（单个自包含文件）
python build.py --open          # 生成并打开
python build.py --serve         # 本地服务 + 保存即重建（推荐审阅时用）
python build.py --dump          # 只打印模型摘要，不出 HTML
python build.py --note 05       # 换一份 note（只有通用视图）
```

`--serve` 提供 3 个额外能力：

| 端点 | 作用 |
|---|---|
| `GET /api/events` | SSE。note / web/ / review/ 任一文件变化 → 重建并把新模型推给浏览器 |
| `POST /api/review` | 把批注列表写回 `review/06.json`（按原文片段锚定，不按行号） |
| `GET /api/model` | 当前模型 JSON，方便脚本消费 |

## 八个视图

| 页 | 内容 |
|---|---|
| 总览 | 统计、需要先看的错误/警告、命令一览表、疏漏+待决、需求覆盖、章节地图 |
| 结构 | 按章节读原文 + 结构度量（字数、⚠ 数、"为什么"段、被引用次数） |
| 命令 | 每条命令一条"请求帧 → 响应帧"链：帧长推算、字段表、线格式字节、模式要求、超时、可能状态码 |
| 报文 | 帧格式两张表、两个真实往返的逐字节图、FRAG 语义、TAG=0xFF 主动上报、状态码引用矩阵 |
| 代码 | 文档里每个代码符号在工程里的落点（code / sdk / proto / planned / comment / missing）+ 行号引用校验 |
| 关系 | 疏漏 ↔ 引用矩阵、需求 → 命令、代码符号分布、孤立章节、悬空引用 |
| 问题 | checks.py 的全部输出，按严重度排序，附重复表述与 §12 待办清单 |
| 原文 | 带行号的原文，双击任意段落加批注 |

快捷键：`/` 打开搜索（命令名 / 码 / 章节号 / 正文全文），`Esc` 关闭。

## 数据来源与"权威性"

`build.py` 只读两份东西：note 的 markdown 和 `dev/` 下的代码。所有结论都是这两者的差集：

- **字段布局 / 帧长 / 帧数** ← 段落声明 vs 表格合计，两边都是原文里的数字；
- **代码锚点** ← 反引号里的标识符，去 `app/sensor_beacon`、`sensor_tool`、`desktop`、`tool` 里找，抓不到就是 `missing`；
- **行号引用** ← `main.c:857` 这种引用，去文件里看那一行现在是什么；
- 疏漏、需求、待办、待决全部来自 note 自己的表格，不另外维护清单。

因此 HTML 里报出的问题**不是人工备注**，是每次构建重新算出来的 —— 改了 note 一保存，问题清单就跟着变。

## 已知的口径

- `SENSOR` 响应声明 26 字节、字段表合计 24 字节（漏了 `sample_tick` 那 4 字节的偏移 20 没写进表里）—— 报文页那两张逐字节图会把多出来的字节标成 `?`。
- `CFG_SET` 文中写 4~13，按 mask 位表推算 4~12 —— 总览页的命令表里并排显示两个值。
- `ERR_NOT_CALIBRATED` 只在 §3 出现，没有命令会返回它。
- `main.c:857` 已经过期，现在那一行是 `GPREGRET2`。

## 批注

`review/06.json` 由 `--serve` 写入，格式：

```json
[{ "match": "原文里的一句片段", "text": "要改什么", "author": "reviewer" }]
```

按片段锚定而不是行号，所以 note 增删行之后批注不会错位；片段找不到时标记为"锚点已失效"而不是静默丢弃。
