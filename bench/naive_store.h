/*
 * Baseline: naive in-place key-value store, the "struct in a flash sector"
 * pattern. Each key has a home sector chosen by hash; an update reads the
 * sector into RAM, erases it and programs it back. Not power-safe and used
 * only as the wear/write-amplification baseline in the benchmark.
 */
#ifndef NAIVE_STORE_H
#define NAIVE_STORE_H

#include <stddef.h>
#include "pfkv.h"

#define NAIVE_MAX_SECTOR 8192

typedef struct {
    const pfkv_flash_t *flash;
    uint8_t buf[NAIVE_MAX_SECTOR];
} naive_store_t;

int naive_init(naive_store_t *s, const pfkv_flash_t *flash);
int naive_put(naive_store_t *s, const char *key, const void *val, size_t len);
int naive_get(naive_store_t *s, const char *key, void *val, size_t cap, size_t *len);

#endif
