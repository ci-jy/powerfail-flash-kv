#!/usr/bin/env python3
"""Boot the FreeRTOS image in QEMU mps2-an385 and check the UART for PASS.

The UART log is saved next to the ELF as qemu_uart.log (used by footprint.py).
"""
import pathlib
import subprocess
import sys

TIMEOUT_S = 120


def main() -> int:
    elf = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "firmware/build/pfkv_demo.elf")
    cmd = [
        "qemu-system-arm", "-M", "mps2-an385", "-nographic", "-monitor", "none",
        "-serial", "stdio", "-semihosting-config", "enable=on,target=native",
        "-kernel", str(elf),
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT_S)
        out, code = proc.stdout.replace("\r", ""), proc.returncode
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or b"").decode(errors="replace").replace("\r", "")
        code = None
    log = elf.with_name("qemu_uart.log")
    log.write_text(out)
    sys.stdout.write(out)
    if code is None:
        print(f"qemu-test: FAIL (no exit within {TIMEOUT_S}s)")
        return 1
    if "RESULT: PASS" in out and code == 0:
        print("qemu-test: PASS")
        return 0
    print(f"qemu-test: FAIL (exit code {code})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
