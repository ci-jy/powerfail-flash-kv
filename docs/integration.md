# Integrating pfkv into firmware

## Flash HAL

Implement the four-member HAL in `include/pfkv.h` for your part, then mount once at boot:

```c
static int my_read(void *ctx, uint32_t addr, void *buf, uint32_t len);   /* any alignment */
static int my_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len); /* prog_size aligned, 1->0 only */
static int my_erase(void *ctx, uint32_t sector);                          /* whole sector -> 0xFF */

static const pfkv_flash_t flash = {
    .sector_size = 2048, .sector_count = 4, .prog_size = 8,   /* e.g. STM32L4: 2 KiB pages, 64-bit writes */
    .read = my_read, .prog = my_prog, .erase = my_erase,
};
static pfkv_t kv;

pfkv_mount(&kv, &flash, NULL);               /* formats blank flash, repairs interrupted GC */
pfkv_put(&kv, "cal/adc0", &gain, sizeof gain);
pfkv_get(&kv, "cal/adc0", &gain, sizeof gain, NULL);
```

- Addresses passed to the HAL are relative to the start of the partition.
- `prog` addresses and lengths are multiples of `prog_size` (4, 8, 16 or 32). Each program unit is written at most once between erases, except that an all-zero pattern may be written over a programmed unit.
- `sector_count` must be at least 3.
- Each HAL function returns 0 on success and non-zero on failure.

The library does no dynamic allocation and is not thread safe on its own. With FreeRTOS, use `port/freertos/pfkv_os.h`, which wraps each call in a recursive mutex.

## Configuration

`pfkv_mount()` takes an optional `pfkv_config_t`. Passing `NULL` gives greedy GC with `wl_threshold` 64.

- `gc_policy`: `PFKV_GC_GREEDY` reclaims the sector with the fewest live bytes, `PFKV_GC_OLDEST` always reclaims the oldest sector (round-robin).
- `wl_threshold`: when the gap between the highest and lowest erase count goes above this, the least-worn data sector is reclaimed so its cold data moves to a worn sector. 0 disables static wear leveling.

## Sizing sectors

- Live data (keys + values + per-record overhead) can use at most `(sector_count - 2) * (sector_size - header)` bytes. One sector is always kept erased as the GC destination, and one more is headroom.
- Each record costs `align(16 + key + value, prog_size) + prog_size` bytes, so a 4-byte counter with a 10-byte key takes 40 B with 8-byte programming.
- One value must fit in one sector (`pfkv_max_value_len()`).
- More sectors mean fewer erases per write and longer life (see the lifetime column in [design.md](design.md)).

Compile-time limits, which size the `pfkv_t` struct and can be overridden with `-D`:

- `PFKV_MAX_SECTORS` (default 32)
- `PFKV_MAX_KEYS` (default 128)
- `PFKV_MAX_KEY_LEN` (default 64)

## Return codes

- `PFKV_OK` (0)
- `PFKV_ERR_IO` (-1): the flash HAL reported an error, including power loss
- `PFKV_ERR_NOT_FOUND` (-2)
- `PFKV_ERR_NOSPC` (-3): live data or key count exceeds capacity
- `PFKV_ERR_INVAL` (-4): bad argument or geometry
- `PFKV_ERR_CORRUPT` (-5): payload CRC mismatch on read
- `PFKV_ERR_TOOBIG` (-6): value does not fit the caller's buffer
