/*
 * Thread-safe pfkv wrapper for FreeRTOS: every call takes a recursive mutex,
 * so the store can be shared by any number of tasks (and pfkv_os_get may be
 * called from inside a pfkv_os_iterate callback).
 */
#ifndef PFKV_OS_H
#define PFKV_OS_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "pfkv.h"

typedef struct {
    pfkv_t kv;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buf;
} pfkv_os_t;

int pfkv_os_mount(pfkv_os_t *s, const pfkv_flash_t *flash, const pfkv_config_t *cfg);
int pfkv_os_put(pfkv_os_t *s, const char *key, const void *val, size_t len);
int pfkv_os_get(pfkv_os_t *s, const char *key, void *buf, size_t buflen, size_t *out_len);
int pfkv_os_delete(pfkv_os_t *s, const char *key);
int pfkv_os_iterate(pfkv_os_t *s, pfkv_iter_cb cb, void *arg);
void pfkv_os_stats(pfkv_os_t *s, pfkv_stats_t *out);

#endif
