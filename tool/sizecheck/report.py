"""Turn the two link results into a flash budget report.

Reads the .map files produced by build.sh and attributes flash bytes to the
object file each section came from, so the answer says not just "how much is
left" but "what is eating it".
"""
import io
import os
import re
import subprocess
import sys

FLASH_ORIGIN = 0x19000
FLASH_LEN = 0x13000          # 77824 -- from the .uvprojx IROM1 setting
RAM_LEN = 0x3B00             # 15104 -- from the .uvprojx IRAM1 setting

# Group objects into the same buckets the .uvprojx uses, so the numbers can be
# read against the project tree.
def bucket(obj):
    o = obj.lower()
    if any(k in o for k in ("_main_c", "services_", "drivers_")):
        return "本工程代码 (main + services + drivers)"
    if "log" in o or "segger_rtt" in o or "fprintf" in o:
        return "日志 (NRF_LOG + RTT + fprintf)"
    if any(k in o for k in ("ble_", "nrf_ble_", "sdh")):
        return "BLE 协议栈胶水 (nrf_sdh / nus / gatt / qwr / advdata)"
    if "fstorage" in o:
        return "flash 存储 (fstorage)"
    if any(k in o for k in ("nrfx_", "nrf_drv_")):
        return "外设驱动 (nrfx: saadc/gpiote/uarte/clock)"
    if any(k in o for k in ("app_timer", "app_button", "app_scheduler",
                            "app_error", "app_util", "nrf_atomic", "nrf_atfifo",
                            "nrf_atflags", "nrf_balloc", "nrf_memobj",
                            "nrf_section", "nrf_strerror", "nrf_assert",
                            "hardfault", "nrf_pwr_mgmt")):
        return "SDK 基础库 (timer/button/error/atomic/...)"
    if any(k in o for k in ("startup", "system_nrf", "boards", "bsp")):
        return "启动/板级 (startup + system + boards + bsp)"
    return "其它 / libc"


def parse(path):
    """section name -> (size, object) for everything landing in FLASH."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    rows = []
    # Lines look like:  .text.foo   0x0001a2b4    0x2c   /path/to/file.o
    pat = re.compile(
        r"^\s*(\.[\w.$]+)\s*\n?\s*0x([0-9a-f]{8,16})\s+0x([0-9a-f]+)\s+(\S+\.o)",
        re.M)
    for m in pat.finditer(text):
        addr = int(m.group(2), 16)
        size = int(m.group(3), 16)
        obj = m.group(4).rsplit("/", 1)[-1]
        if size and FLASH_ORIGIN <= addr < FLASH_ORIGIN + FLASH_LEN:
            rows.append((size, obj))
    return rows


def report(tag, mapfile, text_sz, data_sz, bss_sz):
    used = text_sz + data_sz
    free = FLASH_LEN - used
    print("=" * 66)
    print("%s" % tag)
    print("=" * 66)
    print("  代码区容量 (IROM1)  : %6d B  = %5.1f KB   (0x19000..0x2C000)"
          % (FLASH_LEN, FLASH_LEN / 1024.0))
    print("  已用 (text + data)  : %6d B  = %5.1f KB   (%.1f%%)"
          % (used, used / 1024.0, 100.0 * used / FLASH_LEN))
    print("  --> 剩余可用        : %6d B  = %5.1f KB   (%.1f%%)"
          % (free, free / 1024.0, 100.0 * free / FLASH_LEN))
    print("  RAM 已用 (data+bss) : %6d B  = %5.1f KB / %.1f KB  (%.1f%%)"
          % (data_sz + bss_sz, (data_sz + bss_sz) / 1024.0, RAM_LEN / 1024.0,
             100.0 * (data_sz + bss_sz) / RAM_LEN))

    rows = parse(mapfile)
    agg = {}
    for size, obj in rows:
        agg.setdefault(bucket(obj), 0)
        agg[bucket(obj)] += size
    tot = sum(agg.values())
    print("\n  flash 占用归因 (map 统计 %d B, 与 size 的差是 libc/对齐):" % tot)
    for k, v in sorted(agg.items(), key=lambda kv: -kv[1]):
        print("    %-52s %6d B  %4.1f KB" % (k, v, v / 1024.0))
    print()


def sizes(elf):
    """Read text/data/bss straight out of the ELF.

    Previously these three numbers were hardcoded here and had to be pasted in
    by hand after every build -- which meant the report could silently describe
    a stale binary. Reading them from the ELF removes that failure mode.
    """
    out = subprocess.run(["arm-none-eabi-size", elf],
                         capture_output=True, text=True, check=True).stdout
    text, data, bss = (int(x) for x in out.strip().split("\n")[1].split()[:3])
    return text, data, bss


HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)

TARGETS = [
    ("Debug 配置  (NRF_LOG_ENABLED=1, UART 日志开)", "debug"),
    ("Release 配置 (NRF_LOG_ENABLED=0, 日志关) <-- 出货用这个", "release"),
]

missing = [t for _, t in TARGETS
           if not (os.path.exists("out/fw_%s.elf" % t)
                   and os.path.exists("out/fw_%s.map" % t))]
if missing:
    sys.stderr.write(
        "缺少构建产物: %s\n"
        "请先跑:  bash build.sh 1 && bash build.sh 0\n"
        "(build.sh 会把每次结果存成 out/fw_debug.* 与 out/fw_release.*)\n"
        % ", ".join(missing))
    raise SystemExit(1)

for title, tag in TARGETS:
    t, d, b = sizes("out/fw_%s.elf" % tag)
    report(title, "out/fw_%s.map" % tag, t, d, b)
