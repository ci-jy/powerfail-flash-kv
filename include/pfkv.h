/*
 * pfkv - power-loss-safe log-structured key-value store for NOR flash.
 *
 * Records are appended to a log that spans a set of equally sized flash
 * sectors. Every record carries a header CRC, a payload CRC and a separately
 * programmed commit unit, so a record becomes visible only after all of its
 * bytes reached flash. Garbage collection copies live records into a fresh
 * sector before the victim is invalidated and erased, and mount rebuilds the
 * in-RAM index from the log, repairing an interrupted garbage collection.
 *
 * The library performs no dynamic allocation and is not thread safe on its
 * own; see port/freertos/pfkv_os.h for a mutex-protected wrapper.
 */
#ifndef PFKV_H
#define PFKV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time limits (override with -D). They size the pfkv_t struct. */
#ifndef PFKV_MAX_SECTORS
#define PFKV_MAX_SECTORS 32
#endif
#ifndef PFKV_MAX_KEYS
#define PFKV_MAX_KEYS 128
#endif
#ifndef PFKV_MAX_KEY_LEN
#define PFKV_MAX_KEY_LEN 64
#endif

/* Return codes. */
enum {
    PFKV_OK = 0,
    PFKV_ERR_IO = -1,       /* flash HAL reported an error (incl. power loss) */
    PFKV_ERR_NOT_FOUND = -2,
    PFKV_ERR_NOSPC = -3,    /* live data or key count exceeds capacity */
    PFKV_ERR_INVAL = -4,    /* bad argument or geometry */
    PFKV_ERR_CORRUPT = -5,  /* payload CRC mismatch on read */
    PFKV_ERR_TOOBIG = -6,   /* value does not fit the caller's buffer */
};

/*
 * Flash HAL. Implement these four members for your part.
 *  - read:  any address/length.
 *  - prog:  addr and len are multiples of prog_size; may only clear bits.
 *           Each prog unit is programmed at most once between erases, except
 *           that an all-zero pattern may be written over a programmed unit.
 *  - erase: sets the whole sector to 0xFF.
 * Each function returns 0 on success, non-zero on failure.
 */
typedef struct pfkv_flash {
    uint32_t sector_size;  /* bytes per erasable sector */
    uint32_t sector_count; /* sectors dedicated to the store (>= 3) */
    uint32_t prog_size;    /* program granularity: 4, 8, 16 or 32 */
    void *ctx;
    int (*read)(void *ctx, uint32_t addr, void *buf, uint32_t len);
    int (*prog)(void *ctx, uint32_t addr, const void *buf, uint32_t len);
    int (*erase)(void *ctx, uint32_t sector);
} pfkv_flash_t;

typedef enum {
    PFKV_GC_OLDEST = 0, /* round-robin: always reclaim the oldest sector */
    PFKV_GC_GREEDY = 1, /* reclaim the sector with the fewest live bytes */
} pfkv_gc_policy_t;

typedef struct pfkv_config {
    pfkv_gc_policy_t gc_policy;
    /* Static wear leveling: when (max - min) erase count exceeds this, the
     * least-worn data sector is reclaimed so its cold data moves to a worn
     * sector and it rejoins the free pool. 0 disables static leveling. */
    uint32_t wl_threshold;
} pfkv_config_t;

typedef struct pfkv_stats {
    uint64_t user_bytes;  /* key + value bytes accepted by put/delete */
    uint64_t prog_bytes;  /* bytes programmed, headers and GC copies included */
    uint64_t erases;
    uint64_t gc_runs;
    uint64_t gc_copied_bytes;
} pfkv_stats_t;

typedef struct pfkv_slot {
    uint32_t addr; /* record address; bit 31 set = tombstone */
    uint32_t hash;
} pfkv_slot_t;

typedef struct pfkv {
    const pfkv_flash_t *flash;
    pfkv_config_t cfg;
    uint32_t hdr_a_size, hdr_b_size, data_start;
    uint32_t max_seq;
    uint32_t active;     /* sector currently appended to */
    uint32_t write_off;  /* next free offset inside the active sector */
    uint32_t nkeys;
    uint32_t erase_count[PFKV_MAX_SECTORS];
    uint32_t seq[PFKV_MAX_SECTORS];       /* 0 = sector not holding data */
    uint32_t live_bytes[PFKV_MAX_SECTORS];
    uint8_t state[PFKV_MAX_SECTORS];
    pfkv_slot_t slots[PFKV_MAX_KEYS];
    pfkv_stats_t stats;
    int mounted;
} pfkv_t;

/* Mount the store, formatting blank flash and repairing interrupted work.
 * cfg may be NULL for defaults (greedy GC, wl_threshold 64). */
int pfkv_mount(pfkv_t *kv, const pfkv_flash_t *flash, const pfkv_config_t *cfg);

/* Durably store a value. When this returns PFKV_OK the value survives any
 * later power loss. If power fails during the call, after remount the key
 * holds either the previous value or the new one, never a mix. */
int pfkv_put(pfkv_t *kv, const char *key, const void *val, size_t len);

/* Copy the value into buf. *out_len (optional) receives the stored length. */
int pfkv_get(pfkv_t *kv, const char *key, void *buf, size_t buflen, size_t *out_len);

/* Durably delete a key (writes a tombstone). PFKV_ERR_NOT_FOUND if absent. */
int pfkv_delete(pfkv_t *kv, const char *key);

/* Visit every live key. Return non-zero from cb to stop early. */
typedef int (*pfkv_iter_cb)(const char *key, size_t val_len, void *arg);
int pfkv_iterate(pfkv_t *kv, pfkv_iter_cb cb, void *arg);

/* Number of live keys. */
uint32_t pfkv_count(const pfkv_t *kv);

/* Largest value that fits for a given key length. */
size_t pfkv_max_value_len(const pfkv_t *kv, size_t key_len);

uint32_t pfkv_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
