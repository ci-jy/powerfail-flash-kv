#include "pfkv_os.h"

#define LOCK(s) xSemaphoreTakeRecursive((s)->lock, portMAX_DELAY)
#define UNLOCK(s) xSemaphoreGiveRecursive((s)->lock)

int pfkv_os_mount(pfkv_os_t *s, const pfkv_flash_t *flash, const pfkv_config_t *cfg)
{
    if (!s->lock)
        s->lock = xSemaphoreCreateRecursiveMutexStatic(&s->lock_buf);
    LOCK(s);
    int rc = pfkv_mount(&s->kv, flash, cfg);
    UNLOCK(s);
    return rc;
}

int pfkv_os_put(pfkv_os_t *s, const char *key, const void *val, size_t len)
{
    LOCK(s);
    int rc = pfkv_put(&s->kv, key, val, len);
    UNLOCK(s);
    return rc;
}

int pfkv_os_get(pfkv_os_t *s, const char *key, void *buf, size_t buflen, size_t *out_len)
{
    LOCK(s);
    int rc = pfkv_get(&s->kv, key, buf, buflen, out_len);
    UNLOCK(s);
    return rc;
}

int pfkv_os_delete(pfkv_os_t *s, const char *key)
{
    LOCK(s);
    int rc = pfkv_delete(&s->kv, key);
    UNLOCK(s);
    return rc;
}

int pfkv_os_iterate(pfkv_os_t *s, pfkv_iter_cb cb, void *arg)
{
    LOCK(s);
    int rc = pfkv_iterate(&s->kv, cb, arg);
    UNLOCK(s);
    return rc;
}

void pfkv_os_stats(pfkv_os_t *s, pfkv_stats_t *out)
{
    LOCK(s);
    *out = s->kv.stats;
    UNLOCK(s);
}
