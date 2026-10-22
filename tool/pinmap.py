"""Report which nRF52810 GPIOs are free, reading the real sources of truth.

Every "used" entry below traces to a file in this repo, not to memory:
  - firmware pin macros ....... dev/app/sensor_beacon/{main.c,drivers/*.h}
  - UART log pin .............. pca10040e/s112/config/sdk_config.h
  - LFCLK source .............. same sdk_config.h (NRF_SDH_CLOCK_LF_SRC)
  - reset pin ................. CONFIG_GPIO_AS_PINRESET in .uvprojx
                                + NRF_UICR->PSELRESET = 21 in system_nrf52810.c
  - DK peripherals ............ components/boards/pca10040.h
  - AIN mapping ............... nRF52810 has AIN0..7 on P0.02-05 / P0.28-31

Run:  python pinmap.py
"""
import io
import os
import re
import sys

DEV = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.dirname(DEV)
APP = os.path.join(DEV, "app", "sensor_beacon")
CFG = os.path.join(APP, "pca10040e", "s112", "config", "sdk_config.h")
PROJ = os.path.join(APP, "pca10040e", "s112", "arm5_no_packs",
                    "ble_app_beacon_pca10040e_s112.uvprojx")


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


# ---- 1. scrape the actual pin assignments ------------------------------
used = {}          # pin -> "what uses it"


def claim(pin, who):
    pin = int(pin)
    if pin in used:
        used[pin] += " + " + who
    else:
        used[pin] = who


src = read(os.path.join(APP, "main.c"))
for name, pin in re.findall(r"#define\s+(\w*_PIN\w*)\s+(\d+)", src):
    claim(pin, name)
for hdr in ("drv_key.h", "drv_led.h"):
    h = read(os.path.join(APP, "drivers", hdr))
    for name, pin in re.findall(r"#define\s+(\w*_PIN\w*)\s+(\d+)", h):
        claim(pin, name)

cfg = read(CFG)
m = re.search(r"#define NRF_LOG_BACKEND_UART_TX_PIN (\d+)", cfg)
if m:
    claim(m.group(1), "UART log TX")

# LFCLK: 1 = XTAL -> the 32.768 kHz crystal sits on P0.00/P0.01
lf = re.search(r"#define NRF_SDH_CLOCK_LF_SRC (\d+)", cfg)
lf_xtal = lf and lf.group(1) == "1"
if lf_xtal:
    claim(0, "LF XTAL")
    claim(1, "LF XTAL")

# Reset pin: enabled by a compile define, pin number lives in system_nrf52810.c
proj = read(PROJ)
defines = re.search(r"<Define>([^<]*)</Define>", proj).group(1)
pinreset = "CONFIG_GPIO_AS_PINRESET" in defines
sysc = read(os.path.join(SDK, "modules", "nrfx", "mdk", "system_nrf52810.c"))
mr = re.search(r"NRF_UICR->PSELRESET\[0\] = (\d+)", sysc)
reset_pin = int(mr.group(1)) if mr else None
if pinreset and reset_pin is not None:
    claim(reset_pin, "RESET (CONFIG_GPIO_AS_PINRESET)")

# ---- 2. constraints that do not "use" a pin but restrict it -----------
dk = read(os.path.join(SDK, "components", "boards", "pca10040.h"))
dk_pins = {}
for name, pin in re.findall(r"#define\s+(LED_[1-4]|BUTTON_[1-4])\s+(\d+)", dk):
    dk_pins.setdefault(int(pin), []).append(name)
for name, pin in re.findall(
        r"#define\s+((?:RX|TX|CTS|RTS)_PIN_NUMBER)\s+(\d+)", dk):
    dk_pins.setdefault(int(pin), []).append("DK " + name.split("_")[0])

# E73-TBM module: from main.c's own documented survey (kept as the note says)
e73 = {13: "按键", 14: "按键", 17: "LED", 18: "LED",
       5: "CH340 UART", 6: "CH340 UART", 7: "CH340 UART", 8: "CH340 UART",
       9: "NFC 焊盘", 10: "NFC 焊盘", 21: "复位"}

AIN = {2: "AIN0", 3: "AIN1", 4: "AIN2", 5: "AIN3",
       28: "AIN4", 29: "AIN5", 30: "AIN6", 31: "AIN7"}
# Datasheet Table 100: P0.25..P0.29 are "low drive, low frequency I/O only"
RF_NEAR = set(range(25, 30))

# ---- 3. report --------------------------------------------------------
print("nRF52810 / E73-TBM  引脚占用表   (P0.00 - P0.31, 共 %d 脚)" % 32)
print("LFCLK = %s   PINRESET = %s (P0.%02d)"
      % ("XTAL(占 P0.00/01)" if lf_xtal else "内部 RC(P0.00/01 可释放)",
         "on" if pinreset else "off", reset_pin or -1))
print()
hdr = "%-6s %-34s %-22s %-18s" % ("引脚", "本工程占用", "DK(pca10040) 冲突", "E73-TBM 冲突")
print(hdr)
print("-" * len(hdr))

free_both, free_e73_only, free_restricted = [], [], []
for p in range(32):
    u = used.get(p, "")
    d = "/".join(dk_pins.get(p, [])) or ""
    e = e73.get(p, "")
    tags = []
    if p in AIN:
        tags.append(AIN[p])
    if p in RF_NEAR:
        tags.append("低速 only")
    note = (" [" + ",".join(tags) + "]") if tags else ""
    print("%-6s %-34s %-22s %-18s" % ("P0.%02d" % p, u + note, d, e))
    if u:
        continue
    if p in RF_NEAR:
        free_restricted.append(p)
    elif d and e:
        pass
    elif e:
        pass
    elif d:
        free_e73_only.append(p)
    else:
        free_both.append(p)

fmt = lambda L: ", ".join("P0.%02d" % p for p in L) or "(无)"
print()
print("空闲且两板都安全            : %s" % fmt(free_both))
print("空闲, 但 DK 上有外设占用    : %s" % fmt(free_e73_only))
print("空闲, 但仅限低速信号(25-29) : %s" % fmt(free_restricted))
print()
print("已用 %d 脚, 空闲 %d 脚" % (len(used), 32 - len(used)))
print()
print("注意事项:")
print("  * AIN0-7 (P0.02-05 / P0.28-31) 标着 [AINn] 只是提示它【可以】做模拟输入,")
print("    并不代表已被占用。本工程的 SAADC 用的是 NRF_SAADC_INPUT_VDD(内部 VDD),")
print("    不接任何引脚 —— 所以 8 个 AIN 全部可自由支配。")
print("  * P0.25-29 的\"低速 only\"来自数据手册 Table 100(紧邻射频)。慢速电平输入")
print("    可以, 时钟/PWM/SPI 这类高频信号不行。")
print("  * P0.00/01 若把 NRF_SDH_CLOCK_LF_SRC 从 1(XTAL) 改成 0(内部 RC) 即可释放,")
print("    代价是 LFCLK 精度下降(BLE 仍可工作, 但功耗与定时精度变差)。")
print("  * \"DK 上有外设占用\"= 真机可用, 但插在 PCA10040 开发板上台架调试会冲突。")
