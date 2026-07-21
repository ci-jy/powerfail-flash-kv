#include "naive_store.h"

#include <string.h>

/* Sector image: sequence of [klen u8][vlen u16][key][value], klen 0xFF = end. */

static uint32_t home(const naive_store_t *s, const char *key)
{
    uint32_t h = 2166136261u;
    for (; *key; key++)
        h = (h ^ (uint8_t)*key) * 16777619u;
    return h % s->flash->sector_count;
}

int naive_init(naive_store_t *s, const pfkv_flash_t *flash)
{
    if (flash->sector_size > NAIVE_MAX_SECTOR)
        return PFKV_ERR_INVAL;
    s->flash = flash;
    return PFKV_OK;
}

static uint32_t find(const uint8_t *b, uint32_t size, const char *key, uint32_t *end)
{
    uint32_t off = 0, klen = (uint32_t)strlen(key), hit = UINT32_MAX;
    while (off + 3 <= size && b[off] != 0xFF) {
        uint32_t kl = b[off], vl = (uint32_t)b[off + 1] | ((uint32_t)b[off + 2] << 8);
        if (kl == klen && memcmp(b + off + 3, key, klen) == 0)
            hit = off;
        off += 3 + kl + vl;
    }
    *end = off;
    return hit;
}

int naive_put(naive_store_t *s, const char *key, const void *val, size_t len)
{
    const pfkv_flash_t *f = s->flash;
    uint32_t sec = home(s, key), end, klen = (uint32_t)strlen(key);
    if (f->read(f->ctx, sec * f->sector_size, s->buf, f->sector_size))
        return PFKV_ERR_IO;
    uint32_t hit = find(s->buf, f->sector_size, key, &end);
    if (hit != UINT32_MAX) {
        uint32_t old = 3 + s->buf[hit] + ((uint32_t)s->buf[hit + 1] | ((uint32_t)s->buf[hit + 2] << 8));
        memmove(s->buf + hit, s->buf + hit + old, end - hit - old);
        end -= old;
        memset(s->buf + end, 0xFF, f->sector_size - end);
    }
    if (end + 3 + klen + len > f->sector_size)
        return PFKV_ERR_NOSPC;
    s->buf[end] = (uint8_t)klen;
    s->buf[end + 1] = (uint8_t)len;
    s->buf[end + 2] = (uint8_t)(len >> 8);
    memcpy(s->buf + end + 3, key, klen);
    memcpy(s->buf + end + 3 + klen, val, len);
    end += 3 + klen + (uint32_t)len;
    /* Read-modify-erase-write of the whole home sector. */
    uint32_t used = (end + f->prog_size - 1) / f->prog_size * f->prog_size;
    if (f->erase(f->ctx, sec) || f->prog(f->ctx, sec * f->sector_size, s->buf, used))
        return PFKV_ERR_IO;
    return PFKV_OK;
}

int naive_get(naive_store_t *s, const char *key, void *val, size_t cap, size_t *len)
{
    const pfkv_flash_t *f = s->flash;
    uint32_t sec = home(s, key), end;
    if (f->read(f->ctx, sec * f->sector_size, s->buf, f->sector_size))
        return PFKV_ERR_IO;
    uint32_t hit = find(s->buf, f->sector_size, key, &end);
    if (hit == UINT32_MAX)
        return PFKV_ERR_NOT_FOUND;
    uint32_t kl = s->buf[hit], vl = (uint32_t)s->buf[hit + 1] | ((uint32_t)s->buf[hit + 2] << 8);
    if (len)
        *len = vl;
    if (vl > cap)
        return PFKV_ERR_TOOBIG;
    memcpy(val, s->buf + hit + 3 + kl, vl);
    return PFKV_OK;
}
