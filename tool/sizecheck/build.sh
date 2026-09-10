#!/usr/bin/env bash
# Measure the real linked flash footprint of the current firmware.
#
# Why this exists: the project is MDK-only and Keil is not installed here, so we
# cannot get the authoritative armcc number. Linking with arm-none-eabi-gcc using
# the same source list, the same -D defines and the same memory layout gives a
# real linked size -- unlike summing .o files, which grossly overestimates
# because it counts everything the linker would discard.
#
# Reports are GCC numbers. armcc typically produces slightly SMALLER code for
# this kind of SDK build, so treat the result as a conservative (pessimistic)
# estimate of flash used.
set -euo pipefail

# Locate everything relative to this script, so the tool keeps working if the
# tree is moved or cloned elsewhere. (The tree HAS been moved once already --
# see sensor_tool/README.md on the path-with-'&' problem.)
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DEV=$(cd "$HERE/../.." && pwd)          # dev/
SDK=$(cd "$DEV/.." && pwd)              # nRF5_SDK_15.0.0_*/
APP=$DEV/app/sensor_beacon
PROJ=$APP/pca10040e/s112/arm5_no_packs
OUT=$HERE/out
mkdir -p "$OUT"

for d in "$PROJ" "$SDK/modules/nrfx/mdk"; do
  [ -d "$d" ] || { echo "找不到 $d —— 目录结构与预期不符, 请检查本脚本的路径推导" >&2; exit 1; }
done

# ---- include paths: taken from the .uvprojx so they cannot drift ------------
RAW=$(tr -d '\r' < "$PROJ/ble_app_beacon_pca10040e_s112.uvprojx" \
      | grep -o '<IncludePath>[^<]\+</IncludePath>' | head -1 \
      | sed 's|<IncludePath>||; s|</IncludePath>||')
INC=()
IFS=';' read -ra PARTS <<< "$RAW"
for p in "${PARTS[@]}"; do
  [ -z "$p" ] && continue
  INC+=("-I$PROJ/${p//\\//}")
done
INC+=("-I$APP")

# ---- defines: same as the .uvprojx Debug target -----------------------------
DEFS=(-DBOARD_PCA10040 -DCONFIG_GPIO_AS_PINRESET -DDEVELOP_IN_NRF52832
      -DFLOAT_ABI_SOFT -DNRF52810_XXAA -DNRF52_PAN_74 -DNRF_SD_BLE_API_VERSION=6
      -DS112 -DSOFTDEVICE_PRESENT -DSWI_DISABLE0
      -D__HEAP_SIZE=2048 -D__STACK_SIZE=2048)

# NRF_LOG_ENABLED comes from the target: Debug=1, Release=0. Passed in by caller.
LOGDEF=${1:-1}
DEFS+=(-DNRF_LOG_ENABLED=$LOGDEF)
[ "$LOGDEF" = "1" ] && DEFS+=(-DNRF_LOG_DEFERRED=0)

CFLAGS=(-mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=soft
        -Os -std=c99 -Wall
        -ffunction-sections -fdata-sections -fno-strict-aliasing
        -fno-builtin -fshort-enums)

# ---- source list: pulled from the .uvprojx, with the 3 Keil-only files
#      swapped for their GCC counterparts (armasm startup cannot be assembled
#      by gcc; the *_keil / *_KEIL variants are compiler-specific shims) -------
# !! Must strip CR: Python writing stdout in text mode on Windows turns
#    \n into \r\n, and `mapfile -t` only strips \n. The leftover \r stays
#    glued to every path and gcc then reports "linker input file not found:
#    Invalid argument", which hints at nothing. Same class of trap as the
#    CRLF .uvprojx.
python "$HERE/srclist.py" "$PROJ/ble_app_beacon_pca10040e_s112.uvprojx" \
    | tr -d '\r' > "$OUT/srclist.txt"
mapfile -t SRC < "$OUT/srclist.txt"

echo "源文件数: ${#SRC[@]}  (NRF_LOG_ENABLED=$LOGDEF)"

OBJS=()
for s in "${SRC[@]}"; do
  abs="$PROJ/$s"
  o="$OUT/$(echo "$s" | tr '/.' '__').o"
  if [[ "$s" == *.S ]]; then
    arm-none-eabi-gcc -c -x assembler-with-cpp "${CFLAGS[@]}" "${DEFS[@]}" \
        "${INC[@]}" -o "$o" "$abs"
  else
    arm-none-eabi-gcc -c "${CFLAGS[@]}" "${DEFS[@]}" "${INC[@]}" \
        -Wno-expansion-to-defined -Wno-unused-function -Wno-unused-variable \
        -Wno-unused-but-set-variable -Wno-implicit-fallthrough \
        -o "$o" "$abs" 2>>"$OUT/warn.log"
  fi
  OBJS+=("$o")
done

# Name the artifacts after the target so report.py can read both without the
# caller having to shuffle files around by hand.
TAG=$([ "$LOGDEF" = "1" ] && echo debug || echo release)

arm-none-eabi-gcc "${CFLAGS[@]}" -T"$HERE/link.ld" \
    -L"$SDK/modules/nrfx/mdk" \
    -Wl,--gc-sections -Wl,-Map="$OUT/fw_$TAG.map" --specs=nano.specs \
    -o "$OUT/fw_$TAG.elf" "${OBJS[@]}" -lc -lnosys -lm

arm-none-eabi-size "$OUT/fw_$TAG.elf"

# The .o files are per-target; leaving them behind would let the next run link a
# mix of debug and release objects.
rm -f "$OUT"/*.o
