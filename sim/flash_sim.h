/*
 * NOR flash simulator with power-cut injection.
 *
 * Semantics enforced:
 *  - erase sets a whole sector to 0xFF;
 *  - program can only clear bits (result = old & new);
 *  - program address and length must be multiples of prog_size;
 *  - each prog unit is programmed at most once between erases, except that
 *    writing an all-zero pattern over a programmed unit is allowed.
 * Violations make the call fail and are counted.
 *
 * Every program and erase is one "operation". When the operation counter
 * reaches cut_at, power is lost: the operation either has no effect or is
 * torn (a random prefix completes and one byte is partially changed), the
 * device goes dead and on_cut is invoked (the harness longjmps out of it).
 * All later calls fail until flash_sim_power_on().
 */
#ifndef FLASH_SIM_H
#define FLASH_SIM_H

#include <stdint.h>
#include "pfkv.h"

#define FLASH_SIM_NO_CUT UINT64_MAX

typedef enum { CUT_CLEAN = 0, CUT_TORN = 1 } flash_cut_mode_t;

typedef struct flash_sim {
    uint8_t *mem;
    uint8_t *unit_programmed; /* one byte per prog unit */
    uint32_t *erase_counts;   /* one per sector */
    uint32_t sector_size, sector_count, prog_size;
    uint64_t ops;
    uint64_t cut_at;
    flash_cut_mode_t cut_mode;
    uint32_t rng;
    int dead;
    uint32_t violations;
    uint64_t prog_bytes, erases;
    void (*on_cut)(void *arg);
    void *cut_arg;
} flash_sim_t;

/* Buffers: mem = sector_size*sector_count bytes, unit_programmed =
 * mem size / prog_size bytes, erase_counts = sector_count words. */
void flash_sim_init(flash_sim_t *f, uint8_t *mem, uint8_t *unit_programmed, uint32_t *erase_counts,
                    uint32_t sector_size, uint32_t sector_count, uint32_t prog_size);

/* Restore power; the operation counter is reset and no cut is armed. */
void flash_sim_power_on(flash_sim_t *f);

/* Arm a cut at the n-th upcoming operation (0 = the very next one). */
void flash_sim_arm_cut(flash_sim_t *f, uint64_t n, flash_cut_mode_t mode, uint32_t seed);

/* HAL descriptor to hand to pfkv_mount(). */
pfkv_flash_t flash_sim_hal(flash_sim_t *f);

#endif
