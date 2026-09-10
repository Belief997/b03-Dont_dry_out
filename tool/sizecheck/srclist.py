"""Emit the source list from the .uvprojx, with Keil-only files swapped for GCC.

Kept as its own file rather than a heredoc inside build.sh: nesting a heredoc
inside a process substitution inside mapfile was silently producing a broken
array, which showed up as gcc treating .c files as linker inputs.
"""
import io
import re
import sys

PROJ = sys.argv[1]

# armasm startup cannot be assembled by gcc; the *_keil / *_KEIL files are
# compiler-specific shims that have GCC counterparts shipped in the same SDK dir.
SWAP = {
    "arm_startup_nrf52810.s": "gcc_startup_nrf52810.S",
    "app_error_handler_keil.c": "app_error_handler_gcc.c",
    "SEGGER_RTT_Syscalls_KEIL.c": "SEGGER_RTT_Syscalls_GCC.c",
}

text = io.open(PROJ, encoding="utf-8", errors="replace").read().replace("\r", "")
groups = text.split("<Groups>")[1].split("</Groups>")[0]

for path in re.findall(r"<FilePath>([^<]+)</FilePath>", groups):
    path = path.replace(chr(92), "/")
    if path.endswith(".h"):
        continue
    base = path.rsplit("/", 1)[-1]
    if base in SWAP:
        path = path[: -len(base)] + SWAP[base]
    sys.stdout.write(path + "\n")
