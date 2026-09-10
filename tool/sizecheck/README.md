# sizecheck —— flash 占用测量工具（**不是**第二套构建系统）

⚠ **本工程只维护 MDK5（`arm5_no_packs`）。这个目录不产出固件，只用来量 flash。**
不要把它当 GCC 构建目标，也不要往里加编译选项去"优化产物"——它唯一的用途是回答
"还剩多少 flash 能加新功能"。

## 为什么要它

本机没装 Keil（`armcc`/`armlink`/`fromelf` 都不在），`_build/` 是空的，全树也没有
`.map`/`.axf`，所以拿不到 armcc 的权威数字。而"把各个 `.o` 的大小加起来"会严重
高估——那样会把链接器本该丢掉的东西全算上。

于是用 `arm-none-eabi-gcc` 按**同一份源文件清单、同一组 `-D` 宏、同一套内存布局**
真实链接一次，取 `arm-none-eabi-size` 的结果。

## 怎么用

```bash
bash build.sh 1     # Debug  配置 (NRF_LOG_ENABLED=1)
bash build.sh 0     # Release 配置 (NRF_LOG_ENABLED=0, 出货用)
python report.py    # 读两次的 .map, 出预算报告 + 占用归因
```

两个 `build.sh` 都要跑：`report.py` 需要 `out/fw_debug.*` 与 `out/fw_release.*`
两套产物，缺哪个会直接报错提示。text/data/bss 由它自己从 ELF 读，不需要手抄。

## 四个文件

| 文件 | 作用 |
|---|---|
| `build.sh` | 编译 + 链接 + `size`；参数 1=Debug、0=Release |
| `srclist.py` | 从 `.uvprojx` 抽源文件清单，并把 3 个 Keil 专用文件换成 GCC 版 |
| `link.ld` | 复刻 `.uvprojx` 的内存布局 + SDK 的 section 变量块 |
| `report.py` | 预算与归因报告；text/data/bss 直接读 ELF |

## 结果的可信度（必须一起看）

GCC 的数字**不等于** armcc 的数字：

- armcc 对这类 SDK 代码通常比 GCC **略小**（经验值 5~15%），所以这里报出的
  "已用"偏保守，真实剩余空间应当**比报告的更多一点**；
- 本脚本用 newlib-nano（`--specs=nano.specs`），MDK 用的是 ARM 自己的 C 库，
  libc 部分不可直接比；
- 两边都开了函数级回收（GCC `--gc-sections`，armlink 默认 `--remove`）。

结论：**当"还剩多少"这个量级判断用足够；不要拿它去卡最后几百字节。**
真要精确数字，在装了 Keil 的机器上编一次，看 `.map` 的 Program Size 行。

## 踩过的三个坑

1. **`arm_startup_nrf52810.s` 是 armasm 语法**，GCC 汇编不了。必须换成同目录的
   `gcc_startup_nrf52810.S`。同理 `app_error_handler_keil.c` →
   `app_error_handler_gcc.c`，`SEGGER_RTT_Syscalls_KEIL.c` → `..._GCC.c`。
2. **`nrf_common.ld` 里没有 SDK 的 section 变量块**，直接链接会报一堆
   `undefined reference to __start_log_const_data` 之类。那些块在 SDK 每个例程
   自己的 `.ld` 里，`link.ld` 已按需抄了 9 个段（`log_dynamic_data` / `fs_data`
   在 RAM，其余在 FLASH）。
3. **Python 在 Windows 上以文本模式写 stdout 会把 `\n` 翻成 `\r\n`**，而
   `mapfile -t` 只剥 `\n`。残留的 `\r` 跟在路径末尾，gcc 报
   `linker input file not found: Invalid argument`，完全看不出原因。
   `build.sh` 里已加 `tr -d '\r'`。

## 顺带说明

`out/` 是构建产物，可以随时删（已在 `dev/.gitignore` 里忽略）。
