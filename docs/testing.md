# Testing

```sh
make test       # 20 Unity tests (ASan + UBSan): API, simulator rules, GC, capacity, CRC, wear leveling, tombstones
make powercut   # exhaustive power-cut run over 3 workloads; non-zero exit on any failure
make qemu-test  # builds the FreeRTOS image, boots it in QEMU mps2-an385, requires "RESULT: PASS" on the UART
```

## The NOR simulator

`sim/flash_sim.c`:

- Enforces 1->0-only programming, aligned program units, one program per unit between erases (all-zero overwrite allowed) and whole-sector erase.
- Can cut power at the Nth operation, either cleanly or as a torn operation. A torn program completes a random prefix plus one partly programmed byte. A torn erase either clears a random prefix or flips random bits to 1 across the whole sector.
- The same code backs the RAM flash driver in the FreeRTOS image and the `pfkv_cli` tool.

## What `make powercut` checks

For each workload it counts the flash operations of a clean run, then replays the run with a cut at every operation: once as a clean cut and once as a torn one. After each cut it remounts and checks that:

- every acknowledged write has its exact value;
- the write that was in flight is either fully applied or not applied at all;
- no other key changed;
- the simulator saw no rule violations;
- the store still accepts and persists new writes.

If the recovery mount itself programs or erases flash (when it repairs an interrupted GC), power is cut again at each of those operations ("nested cuts").

| Workload | Flash ops | Cuts (clean + torn) | Nested cuts | Lost acked writes | Half-visible writes | Bad mounts |
|---|---:|---:|---:|---:|---:|---:|
| config-burst (16 keys, bursts, deletes) | 3,714 | 7,428 | 2,056 | 0 | 0 | 0 |
| counters (4 skewed monotonic counters) | 7,227 | 14,454 | 1,513 | 0 | 0 | 0 |
| mixed-sizes (1-48 B keys, 0-300 B values) | 3,262 | 6,524 | 676 | 0 | 0 | 0 |

32,651 cuts in total, 0 failures. Raw data is in `docs/powercut_results.json`, including the geometry each workload uses.

The harness and tests were checked against deliberately broken builds. If header B is not invalidated before an erase, the harness reports over 600k failures. If tombstones are dropped from a sector other than the oldest, the tombstone unit test fails.

## What `make qemu-test` runs

- Two writer tasks (1,500 counter updates; 400 config updates with deletes) and a reader task share one store through the mutex API.
- The reader checks that the counter never goes backwards and that no config value is ever seen half-written.
- A supervisor task then remounts the flash from scratch (as after a reset), checks every key and prints stack high-water marks.

`make footprint` runs the QEMU test and writes `docs/footprint.md`. High-water marks can differ by a few dozen bytes between runs, because task interleaving depends on preemption timing.
