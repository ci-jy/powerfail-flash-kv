# Design

## On-flash format

Each sector starts with header A (magic, erase count, CRC), written right after an erase, and header B (sequence number + CRC), written when the sector joins the log. After the headers, records are only ever appended.

A record is a 16-byte header with its own CRC, then the key and value with a payload CRC, then a separately programmed commit unit. The newest valid record for a key wins (by sector sequence, then by offset).

## Atomic commit

If power drops while a record is being programmed, the record has a bad CRC or no commit unit and is skipped at mount, so the previous value stays visible. If the header itself is torn, the record length can't be trusted, so mount treats the rest of that sector as used and moves on to a new sector.

The commit unit is partly redundant with the payload CRC in the current failure model. It still guarantees that a record whose data happens to pass the CRC is not treated as valid unless the final program step finished.

## Garbage collection

When the active sector is full, the least-worn free sector becomes active. If no free sector is left, GC picks a victim: the sector with the fewest live bytes (greedy) or the oldest (round-robin). It copies the victim's live records forward, then zeroes the victim's header B and only then erases the victim.

A tombstone is dropped only when its sector is the oldest, so a deleted key cannot come back from an older sector.

## Mount-time recovery

Mount sorts sectors into data, free, blank and dirty. If every sector holds data, power was lost in the middle of GC. In that case the newest sector holds only copies of records that still exist in their source sector, so mount discards it and scans again. Redoing that GC costs one extra sector erase.

The RAM index (hash + address per key) is rebuilt by replaying sectors from oldest to newest. Lookups compare keys by reading them back from flash, and `get` re-checks the payload CRC on every read.

If an interrupted erase destroys header A, that sector's erase count is lost. It is set to the highest known count, which errs toward treating the sector as worn.

## Static wear leveling

If the erase-count gap between the most-worn sector and the least-worn data sector goes above `wl_threshold`, that cold sector becomes the GC victim. Its static data moves to a worn sector, and the cold sector goes back into the free pool.

## Wear benchmark

`make bench` writes `docs/wear_results.json` and `docs/wear_results.png`. Eight 4 KiB sectors, 10k-cycle endurance:

| Store | Erase spread after 200k writes | Write amplification | Writes until first sector wears out |
|---|---:|---:|---:|
| Naive in-place (read-modify-erase-write) | 123,068 | 59.9 | 16,232 |
| pfkv, round-robin GC | 1 | 5.5 | 6,250,154 |
| pfkv, greedy GC, no static leveling | 345 | 4.3 | 5,968,517 |
| pfkv, greedy GC + static wear leveling | 31 | 4.4 | 7,917,922 |

Write amplification is dominated by per-record overhead for tiny values: the 16-byte header, padding and the commit unit.

## Repository layout

```
include/pfkv.h          API + flash HAL interface
src/pfkv.c              the store
sim/flash_sim.[ch]      NOR simulator with power-cut injection
tests/test_pfkv.c       Unity tests
tools/powercut.c        exhaustive power-cut harness
bench/                  wear benchmark + naive in-place baseline
port/freertos/          mutex-protected wrapper
firmware/               FreeRTOS image for QEMU mps2-an385 (startup, linker script, UART, stress test)
examples/pfkv_cli.c     file-backed command-line tool
scripts/                plotting, QEMU check, footprint report, toolchain setup
```

## Third-party code

- `third_party/freertos/`: FreeRTOS Kernel V11.1.0, MIT license (`third_party/freertos/LICENSE.md`).
- `third_party/unity/`: Unity test framework by ThrowTheSwitch.org, MIT license (`third_party/unity/LICENSE.txt`).
