#!/usr/bin/env python3
"""Footprint report: code/RAM of the library for Cortex-M3 (-Os), worst stack
frame from -fstack-usage, and runtime numbers printed by the QEMU firmware.
Writes docs/footprint.md."""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OBJ = ROOT / "firmware" / "build" / "obj"
LOG = ROOT / "firmware" / "build" / "qemu_uart.log"


def size(obj: pathlib.Path):
    out = subprocess.run(["arm-none-eabi-size", str(obj)], capture_output=True, text=True, check=True).stdout
    text, data, bss = out.splitlines()[1].split()[:3]
    return int(text), int(data), int(bss)


def max_frame(su: pathlib.Path):
    best = ("", 0)
    for line in su.read_text().splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and int(parts[1]) > best[1]:
            best = (parts[0].split(":")[-1], int(parts[1]))
    return best


def main() -> int:
    rows = []
    for name, rel in [("pfkv core (src/pfkv.c)", "src/pfkv"), ("FreeRTOS wrapper (pfkv_os.c)", "port/freertos/pfkv_os")]:
        t, d, b = size(OBJ / f"{rel}.o")
        rows.append((name, t, d, b))
    fn, frame = max_frame(OBJ / "src" / "pfkv.su")
    elf = subprocess.run(["arm-none-eabi-size", str(ROOT / "firmware/build/pfkv_demo.elf")],
                         capture_output=True, text=True, check=True).stdout.splitlines()[1].split()

    log = LOG.read_text() if LOG.exists() else ""
    hwm = re.search(r"stack high-water \(bytes free of (\d+)\): (.*)", log)
    ram = re.search(r"sizeof\(pfkv_t\)=(\d+) sizeof\(pfkv_os_t\)=(\d+)", log)
    if not hwm or not ram:
        print("run `make qemu-test` first: UART log missing stack/RAM lines", file=sys.stderr)
        return 1

    md = ["| Component | .text (flash) | .data | .bss |", "|---|---:|---:|---:|"]
    for name, t, d, b in rows:
        md.append(f"| {name} | {t} B | {d} B | {b} B |")
    md.append(f"| Whole demo image (FreeRTOS + tasks + RAM flash) | {elf[0]} B | {elf[1]} B | {elf[2]} B |")
    md += ["", "| RAM / stack | Bytes |", "|---|---:|",
           f"| `pfkv_t` instance (32 sectors, 128 keys max) | {ram.group(1)} |",
           f"| `pfkv_os_t` instance (store + static mutex) | {ram.group(2)} |",
           f"| Largest library stack frame (`{fn}`, -fstack-usage) | {frame} |"]
    total = int(hwm.group(1))
    for task, free in re.findall(r"(\w+)=(\d+)", hwm.group(2)):
        md.append(f"| Task `{task}` peak stack use (FreeRTOS high-water mark) | {total - int(free)} of {total} |")
    text = "\n".join(md) + "\n"
    (ROOT / "docs" / "footprint.md").write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
