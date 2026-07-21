/*
 * pfkv - power-loss-safe log-structured key-value store for NOR flash.
 *
 * Sector layout (P = prog_size):
 *   [A: magic, erase_count, prog_size, crc]  written right after erase
 *   [B: seq, crc(seq)]                       written when the sector joins
 *                                            the log; zeroed before erase
 *   [record][record]...                      appended, never rewritten
 *
 * Record layout:
 *   [hdr 16 B: magic16, key_len, flags, val_len16, rsvd16, data_crc, hdr_crc]
 *   [key][value][0xFF pad to P]  - same program pass as the header
 *   [commit unit, P bytes]       - separate program, written last
 *
 * A record is valid only if hdr_crc, data_crc and the commit unit all check
 * out. The newest record for a key (by sector seq, then offset) wins.
 */
#include "pfkv.h"

#include <string.h>

#define SECT_MAGIC 0x564B4650u /* "PFKV" */
#define REC_MAGIC 0x5AFEu
#define COMMIT_WORD 0xC0DEC0DEu
#define REC_HDR_SIZE 16u
#define TOMB_BIT 0x80000000u
#define FLAG_VALUE 0x01u
#define FLAG_TOMB 0x02u
#define CHUNK 64u

enum { ST_DATA = 0, ST_FREE, ST_FREE_BLANK, ST_DIRTY };

typedef struct {
    uint16_t magic;
    uint8_t key_len;
    uint8_t flags;
    uint16_t val_len;
    uint16_t rsvd;
    uint32_t data_crc;
    uint32_t hdr_crc;
} rec_hdr_t;

typedef struct {
    uint32_t magic, erase_count, prog_size, crc;
} sect_a_t;

/* ---------------------------------------------------------------- utils */

uint32_t pfkv_crc32(uint32_t crc, const void *data, size_t len)
{
    static const uint32_t tab[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ tab[crc & 15u];
        crc = (crc >> 4) ^ tab[crc & 15u];
    }
    return ~crc;
}

static uint32_t hash_key(const char *key, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)key[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t seq_check(uint32_t seq) { return pfkv_crc32(SECT_MAGIC, &seq, 4); }

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

static uint32_t rec_size(const pfkv_t *kv, uint32_t klen, uint32_t vlen)
{
    uint32_t p = kv->flash->prog_size;
    return align_up(REC_HDR_SIZE + klen + vlen, p) + p;
}

static uint32_t sect_addr(const pfkv_t *kv, uint32_t s) { return s * kv->flash->sector_size; }
static uint32_t addr_sector(const pfkv_t *kv, uint32_t a) { return (a & ~TOMB_BIT) / kv->flash->sector_size; }

static int f_read(pfkv_t *kv, uint32_t addr, void *buf, uint32_t len)
{
    return kv->flash->read(kv->flash->ctx, addr, buf, len) ? PFKV_ERR_IO : PFKV_OK;
}

static int f_prog(pfkv_t *kv, uint32_t addr, const void *buf, uint32_t len)
{
    if (kv->flash->prog(kv->flash->ctx, addr, buf, len))
        return PFKV_ERR_IO;
    kv->stats.prog_bytes += len;
    return PFKV_OK;
}

static int is_erased(const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++)
        if (p[i] != 0xFFu)
            return 0;
    return 1;
}

static int range_erased(pfkv_t *kv, uint32_t addr, uint32_t len, int *erased)
{
    uint8_t buf[CHUNK];
    *erased = 1;
    while (len) {
        uint32_t n = len < CHUNK ? len : CHUNK;
        if (f_read(kv, addr, buf, n))
            return PFKV_ERR_IO;
        if (!is_erased(buf, n)) {
            *erased = 0;
            return PFKV_OK;
        }
        addr += n;
        len -= n;
    }
    return PFKV_OK;
}

static void fill_commit(uint8_t *buf, uint32_t p)
{
    uint32_t w = COMMIT_WORD;
    for (uint32_t i = 0; i < p; i += 4)
        memcpy(buf + i, &w, 4);
}

/* ------------------------------------------------------- record parsing */

enum { REC_VALID = 1, REC_SKIP = 0, REC_END = -1 };

/*
 * Parse the record at *off in sector s. Returns REC_VALID (hdr filled),
 * REC_SKIP (torn or uncommitted record, *off advanced past it) or REC_END
 * (*off is where the next record may be written, or sector_size when the
 * tail of the sector cannot be trusted). Negative PFKV error on I/O failure.
 */
static int parse_record(pfkv_t *kv, uint32_t s, uint32_t *off, rec_hdr_t *hdr)
{
    const uint32_t ssz = kv->flash->sector_size, p = kv->flash->prog_size;
    const uint32_t base = sect_addr(kv, s);
    uint8_t buf[CHUNK];
    int rc, erased;

    if (*off + REC_HDR_SIZE + p > ssz) {
        if (*off < ssz) {
            if ((rc = range_erased(kv, base + *off, ssz - *off, &erased)))
                return rc;
            if (!erased)
                *off = ssz;
        }
        return REC_END;
    }
    if ((rc = f_read(kv, base + *off, hdr, REC_HDR_SIZE)))
        return rc;
    if (is_erased(hdr, REC_HDR_SIZE)) {
        /* End of log, unless a torn write left programmed bits further on. */
        if ((rc = range_erased(kv, base + *off, ssz - *off, &erased)))
            return rc;
        if (!erased)
            *off = ssz;
        return REC_END;
    }
    if (hdr->magic != REC_MAGIC || pfkv_crc32(0, hdr, 12) != hdr->hdr_crc ||
        hdr->key_len == 0 || hdr->key_len > PFKV_MAX_KEY_LEN ||
        (hdr->flags != FLAG_VALUE && hdr->flags != FLAG_TOMB)) {
        *off = ssz; /* length unknown: nothing after this point is usable */
        return REC_END;
    }
    uint32_t size = rec_size(kv, hdr->key_len, hdr->val_len);
    if (*off + size > ssz) {
        *off = ssz;
        return REC_END;
    }
    uint32_t start = *off;
    *off += size;

    /* Commit unit must be fully programmed. */
    uint8_t commit[32];
    fill_commit(buf, p);
    if ((rc = f_read(kv, base + start + size - p, commit, p)))
        return rc;
    if (memcmp(commit, buf, p) != 0)
        return REC_SKIP;

    /* Payload CRC over key + value. */
    uint32_t crc = 0, a = base + start + REC_HDR_SIZE, left = (uint32_t)hdr->key_len + hdr->val_len;
    while (left) {
        uint32_t n = left < CHUNK ? left : CHUNK;
        if ((rc = f_read(kv, a, buf, n)))
            return rc;
        crc = pfkv_crc32(crc, buf, n);
        a += n;
        left -= n;
    }
    return crc == hdr->data_crc ? REC_VALID : REC_SKIP;
}

/* ------------------------------------------------------------- the index */

static int read_key(pfkv_t *kv, uint32_t addr, char *key, uint8_t *klen)
{
    rec_hdr_t h;
    int rc = f_read(kv, addr & ~TOMB_BIT, &h, REC_HDR_SIZE);
    if (rc)
        return rc;
    if (h.key_len > PFKV_MAX_KEY_LEN)
        return PFKV_ERR_CORRUPT;
    *klen = h.key_len;
    return f_read(kv, (addr & ~TOMB_BIT) + REC_HDR_SIZE, key, h.key_len);
}

/* Returns slot index, -1 if absent, or a negative PFKV error below -1. */
static int find_slot(pfkv_t *kv, const char *key, size_t klen, uint32_t h, int *out)
{
    char buf[PFKV_MAX_KEY_LEN];
    uint8_t blen;
    *out = -1;
    for (uint32_t i = 0; i < kv->nkeys; i++) {
        if (kv->slots[i].hash != h)
            continue;
        int rc = read_key(kv, kv->slots[i].addr, buf, &blen);
        if (rc)
            return rc;
        if (blen == klen && memcmp(buf, key, klen) == 0) {
            *out = (int)i;
            return PFKV_OK;
        }
    }
    return PFKV_OK;
}

static int slot_size(pfkv_t *kv, uint32_t addr, uint32_t *size)
{
    rec_hdr_t h;
    int rc = f_read(kv, addr & ~TOMB_BIT, &h, REC_HDR_SIZE);
    if (rc)
        return rc;
    *size = rec_size(kv, h.key_len, h.val_len);
    return PFKV_OK;
}

static void remove_slot(pfkv_t *kv, int i)
{
    kv->slots[i] = kv->slots[kv->nkeys - 1];
    kv->nkeys--;
}

/* Point key at the record (addr, size), adjusting live-byte accounting. */
static int index_set(pfkv_t *kv, const char *key, size_t klen, uint32_t addr, uint32_t size, int tomb)
{
    uint32_t h = hash_key(key, klen);
    int i, rc;
    if ((rc = find_slot(kv, key, klen, h, &i)))
        return rc;
    if (i >= 0) {
        uint32_t old;
        if ((rc = slot_size(kv, kv->slots[i].addr, &old)))
            return rc;
        kv->live_bytes[addr_sector(kv, kv->slots[i].addr)] -= old;
    } else {
        if (kv->nkeys >= PFKV_MAX_KEYS)
            return PFKV_ERR_NOSPC;
        i = (int)kv->nkeys++;
        kv->slots[i].hash = h;
    }
    kv->slots[i].addr = addr | (tomb ? TOMB_BIT : 0u);
    kv->live_bytes[addr_sector(kv, addr)] += size;
    return PFKV_OK;
}

/* ------------------------------------------------------ sector management */

static int write_hdr_a(pfkv_t *kv, uint32_t s)
{
    uint8_t buf[32];
    sect_a_t a = { SECT_MAGIC, kv->erase_count[s], kv->flash->prog_size, 0 };
    a.crc = pfkv_crc32(0, &a, 12);
    memset(buf, 0xFF, sizeof buf);
    memcpy(buf, &a, sizeof a);
    return f_prog(kv, sect_addr(kv, s), buf, kv->hdr_a_size);
}

/* Invalidate (if holding data), erase and re-stamp the erase count. */
static int erase_sector(pfkv_t *kv, uint32_t s)
{
    int rc;
    if (kv->state[s] == ST_DATA) {
        uint8_t zero[32];
        memset(zero, 0, sizeof zero);
        if ((rc = f_prog(kv, sect_addr(kv, s) + kv->hdr_a_size, zero, kv->hdr_b_size)))
            return rc;
    }
    kv->state[s] = ST_DIRTY;
    kv->seq[s] = 0;
    kv->live_bytes[s] = 0;
    if (kv->flash->erase(kv->flash->ctx, s))
        return PFKV_ERR_IO;
    kv->erase_count[s]++;
    kv->stats.erases++;
    kv->state[s] = ST_FREE_BLANK;
    if ((rc = write_hdr_a(kv, s)))
        return rc;
    kv->state[s] = ST_FREE;
    return PFKV_OK;
}

static uint32_t free_count(const pfkv_t *kv)
{
    uint32_t n = 0;
    for (uint32_t s = 0; s < kv->flash->sector_count; s++)
        n += kv->state[s] != ST_DATA;
    return n;
}

/* Make the least-worn free sector the new active sector. */
static int take_sector(pfkv_t *kv)
{
    uint32_t best = UINT32_MAX;
    int rc;
    for (uint32_t s = 0; s < kv->flash->sector_count; s++) {
        if (kv->state[s] == ST_DATA)
            continue;
        if (best == UINT32_MAX || kv->erase_count[s] < kv->erase_count[best] ||
            (kv->erase_count[s] == kv->erase_count[best] && kv->state[best] == ST_DIRTY))
            best = s;
    }
    if (best == UINT32_MAX)
        return PFKV_ERR_NOSPC;
    if (kv->state[best] == ST_DIRTY && (rc = erase_sector(kv, best)))
        return rc;
    if (kv->state[best] == ST_FREE_BLANK) {
        if ((rc = write_hdr_a(kv, best)))
            return rc;
        kv->state[best] = ST_FREE;
    }
    uint8_t buf[32];
    uint32_t seq = kv->max_seq + 1u, nseq = seq_check(seq);
    memset(buf, 0xFF, sizeof buf);
    memcpy(buf, &seq, 4);
    memcpy(buf + 4, &nseq, 4);
    if ((rc = f_prog(kv, sect_addr(kv, best) + kv->hdr_a_size, buf, kv->hdr_b_size)))
        return rc;
    kv->max_seq = seq;
    kv->seq[best] = seq;
    kv->state[best] = ST_DATA;
    kv->live_bytes[best] = 0;
    kv->active = best;
    kv->write_off = kv->data_start;
    return PFKV_OK;
}

/* Copy the raw record [src, src+size) to the end of the active sector. */
static int copy_record(pfkv_t *kv, uint32_t src, uint32_t size)
{
    const uint32_t p = kv->flash->prog_size;
    uint32_t dst = sect_addr(kv, kv->active) + kv->write_off, left = size - p;
    uint8_t buf[CHUNK];
    int rc;
    while (left) {
        uint32_t n = left < CHUNK ? left : CHUNK;
        if ((rc = f_read(kv, src, buf, n)) || (rc = f_prog(kv, dst, buf, n)))
            return rc;
        src += n;
        dst += n;
        left -= n;
    }
    fill_commit(buf, p);
    if ((rc = f_prog(kv, dst, buf, p)))
        return rc;
    kv->write_off += size;
    kv->stats.gc_copied_bytes += size;
    return PFKV_OK;
}

static uint32_t pick_victim(pfkv_t *kv)
{
    uint32_t victim = UINT32_MAX, coldest = UINT32_MAX, max_ec = 0;
    for (uint32_t s = 0; s < kv->flash->sector_count; s++) {
        if (kv->erase_count[s] > max_ec)
            max_ec = kv->erase_count[s];
        if (kv->state[s] != ST_DATA || s == kv->active)
            continue;
        if (coldest == UINT32_MAX || kv->erase_count[s] < kv->erase_count[coldest])
            coldest = s;
        if (victim == UINT32_MAX)
            victim = s;
        else if (kv->cfg.gc_policy == PFKV_GC_GREEDY &&
                 kv->live_bytes[s] != kv->live_bytes[victim])
            victim = kv->live_bytes[s] < kv->live_bytes[victim] ? s : victim;
        else if (kv->seq[s] < kv->seq[victim])
            victim = s;
    }
    if (kv->cfg.wl_threshold && coldest != UINT32_MAX &&
        max_ec - kv->erase_count[coldest] > kv->cfg.wl_threshold)
        victim = coldest;
    return victim;
}

/* Relocate the live records of one sector into the active sector, then
 * invalidate and erase it. */
static int gc_one(pfkv_t *kv)
{
    uint32_t v = pick_victim(kv);
    if (v == UINT32_MAX)
        return PFKV_ERR_NOSPC;
    int oldest = 1;
    for (uint32_t s = 0; s < kv->flash->sector_count; s++)
        if (kv->state[s] == ST_DATA && kv->seq[s] < kv->seq[v])
            oldest = 0;

    uint32_t off = kv->data_start, base = sect_addr(kv, v);
    rec_hdr_t h;
    int rc;
    kv->stats.gc_runs++;
    for (;;) {
        uint32_t start = off;
        rc = parse_record(kv, v, &off, &h);
        if (rc == REC_END)
            break;
        if (rc < REC_END)
            return rc;
        if (rc == REC_SKIP)
            continue;
        uint32_t addr = base + start, hh;
        char key[PFKV_MAX_KEY_LEN];
        if ((rc = f_read(kv, addr + REC_HDR_SIZE, key, h.key_len)))
            return rc;
        hh = hash_key(key, h.key_len);
        for (uint32_t i = 0; i < kv->nkeys; i++) {
            if (kv->slots[i].hash != hh || (kv->slots[i].addr & ~TOMB_BIT) != addr)
                continue;
            uint32_t size = off - start;
            if (h.flags == FLAG_TOMB && oldest) {
                /* No older copy of this key can exist anywhere. */
                kv->live_bytes[v] -= size;
                remove_slot(kv, (int)i);
                break;
            }
            uint32_t dst = sect_addr(kv, kv->active) + kv->write_off;
            if ((rc = copy_record(kv, addr, size)))
                return rc;
            kv->slots[i].addr = dst | (kv->slots[i].addr & TOMB_BIT);
            kv->live_bytes[v] -= size;
            kv->live_bytes[kv->active] += size;
            break;
        }
    }
    return erase_sector(kv, v);
}

/* Ensure the active sector has room for `need` bytes. */
static int ensure_space(pfkv_t *kv, uint32_t need)
{
    for (uint32_t attempt = 0; attempt <= kv->flash->sector_count + 1u; attempt++) {
        if (kv->write_off + need <= kv->flash->sector_size)
            return PFKV_OK;
        int rc = take_sector(kv);
        if (rc)
            return rc;
        /* Keep one free sector in reserve as the next GC destination. */
        if (free_count(kv) == 0 && (rc = gc_one(kv)))
            return rc;
    }
    return PFKV_ERR_NOSPC;
}

/* ------------------------------------------------------------------ mount */

static int classify(pfkv_t *kv, uint32_t s, int *ec_known)
{
    const uint32_t base = sect_addr(kv, s);
    sect_a_t a;
    uint32_t b[2];
    int rc, erased;
    *ec_known = 0;
    if ((rc = f_read(kv, base, &a, sizeof a)) || (rc = f_read(kv, base + kv->hdr_a_size, b, 8)))
        return rc;
    int a_ok = a.magic == SECT_MAGIC && a.prog_size == kv->flash->prog_size &&
               pfkv_crc32(0, &a, 12) == a.crc;
    if (a_ok) {
        kv->erase_count[s] = a.erase_count;
        *ec_known = 1;
    }
    int b_ok = b[1] == seq_check(b[0]) && b[0] != 0u && b[0] != 0xFFFFFFFFu;
    if (a_ok && b_ok) {
        kv->state[s] = ST_DATA;
        kv->seq[s] = b[0];
        if (b[0] > kv->max_seq)
            kv->max_seq = b[0];
        return PFKV_OK;
    }
    /* Free only if everything after header A reads erased, padding included. */
    kv->state[s] = ST_DIRTY;
    if ((rc = range_erased(kv, base + kv->hdr_a_size, kv->flash->sector_size - kv->hdr_a_size, &erased)))
        return rc;
    if (!erased)
        return PFKV_OK;
    if (a_ok) {
        kv->state[s] = ST_FREE;
        return PFKV_OK;
    }
    if ((rc = range_erased(kv, base, kv->hdr_a_size, &erased)))
        return rc;
    if (erased)
        kv->state[s] = ST_FREE_BLANK;
    return PFKV_OK;
}

static int index_record(pfkv_t *kv, uint32_t s, uint32_t start, uint32_t end, const rec_hdr_t *h)
{
    char key[PFKV_MAX_KEY_LEN];
    uint32_t addr = sect_addr(kv, s) + start;
    int rc = f_read(kv, addr + REC_HDR_SIZE, key, h->key_len);
    if (rc)
        return rc;
    return index_set(kv, key, h->key_len, addr, end - start, h->flags == FLAG_TOMB);
}

int pfkv_mount(pfkv_t *kv, const pfkv_flash_t *flash, const pfkv_config_t *cfg)
{
    if (!kv || !flash || !flash->read || !flash->prog || !flash->erase)
        return PFKV_ERR_INVAL;
    uint32_t p = flash->prog_size;
    if ((p != 4 && p != 8 && p != 16 && p != 32) || flash->sector_count < 3 ||
        flash->sector_count > PFKV_MAX_SECTORS || flash->sector_size % p ||
        flash->sector_size < 256)
        return PFKV_ERR_INVAL;

    for (int pass = 0; pass < 3; pass++) {
        memset(kv, 0, sizeof *kv);
        kv->flash = flash;
        kv->cfg.gc_policy = PFKV_GC_GREEDY;
        kv->cfg.wl_threshold = 64;
        if (cfg)
            kv->cfg = *cfg;
        kv->hdr_a_size = align_up(sizeof(sect_a_t), p);
        kv->hdr_b_size = align_up(8, p);
        kv->data_start = kv->hdr_a_size + kv->hdr_b_size;

        uint32_t n = flash->sector_count, max_ec = 0, ndata = 0, newest = 0;
        uint8_t known[PFKV_MAX_SECTORS];
        int rc, k;
        for (uint32_t s = 0; s < n; s++) {
            if ((rc = classify(kv, s, &k)))
                return rc;
            known[s] = (uint8_t)k;
            if (k && kv->erase_count[s] > max_ec)
                max_ec = kv->erase_count[s];
            if (kv->state[s] == ST_DATA) {
                ndata++;
                if (kv->seq[s] >= kv->seq[newest] || kv->state[newest] != ST_DATA)
                    newest = s;
            }
        }
        /* A torn erase loses the stamped count; assume the worst case. */
        for (uint32_t s = 0; s < n; s++)
            if (!known[s])
                kv->erase_count[s] = max_ec;

        if (ndata == 0) {
            if ((rc = take_sector(kv)))
                return rc;
            kv->mounted = 1;
            return PFKV_OK;
        }
        if (ndata == n) {
            /* Power failed during garbage collection: the newest sector only
             * holds copies of records that still exist in their source
             * sector. Discard it and start over. */
            if ((rc = erase_sector(kv, newest)))
                return rc;
            continue;
        }

        /* Replay sectors oldest first so later records override. */
        uint32_t last_seq = 0;
        for (uint32_t i = 0; i < ndata; i++) {
            uint32_t s = UINT32_MAX;
            for (uint32_t t = 0; t < n; t++)
                if (kv->state[t] == ST_DATA && kv->seq[t] > last_seq &&
                    (s == UINT32_MAX || kv->seq[t] < kv->seq[s]))
                    s = t;
            last_seq = kv->seq[s];
            uint32_t off = kv->data_start;
            rec_hdr_t h;
            for (;;) {
                uint32_t start = off;
                rc = parse_record(kv, s, &off, &h);
                if (rc == REC_END)
                    break;
                if (rc < REC_END)
                    return rc;
                if (rc == REC_VALID && (rc = index_record(kv, s, start, off, &h)))
                    return rc;
            }
            kv->active = s;
            kv->write_off = off;
        }
        kv->mounted = 1;
        return PFKV_OK;
    }
    return PFKV_ERR_CORRUPT;
}

/* ------------------------------------------------------------- user API */

static int check_key(const char *key, size_t *klen)
{
    if (!key)
        return PFKV_ERR_INVAL;
    *klen = strlen(key);
    return (*klen == 0 || *klen > PFKV_MAX_KEY_LEN) ? PFKV_ERR_INVAL : PFKV_OK;
}

size_t pfkv_max_value_len(const pfkv_t *kv, size_t key_len)
{
    uint32_t area = kv->flash->sector_size - kv->data_start - kv->flash->prog_size;
    uint32_t max = (area & ~(kv->flash->prog_size - 1u)) - REC_HDR_SIZE - (uint32_t)key_len;
    return max > 0xFFFFu ? 0xFFFFu : max;
}

static uint32_t total_live(const pfkv_t *kv)
{
    uint32_t t = 0;
    for (uint32_t s = 0; s < kv->flash->sector_count; s++)
        t += kv->live_bytes[s];
    return t;
}

static int write_record(pfkv_t *kv, const char *key, size_t klen, const void *val, size_t vlen, uint8_t flags)
{
    const uint32_t p = kv->flash->prog_size;
    uint32_t size = rec_size(kv, (uint32_t)klen, (uint32_t)vlen);
    int rc, i;

    /* Capacity: live data must fit in all but two sectors so that GC can
     * always make progress with one sector held in reserve. */
    uint32_t h = hash_key(key, klen), old = 0;
    if ((rc = find_slot(kv, key, klen, h, &i)))
        return rc;
    if (i >= 0 && (rc = slot_size(kv, kv->slots[i].addr, &old)))
        return rc;
    if (i < 0 && kv->nkeys >= PFKV_MAX_KEYS)
        return PFKV_ERR_NOSPC;
    uint32_t cap = (kv->flash->sector_count - 2u) * (kv->flash->sector_size - kv->data_start);
    if (total_live(kv) - old + size > cap)
        return PFKV_ERR_NOSPC;

    if ((rc = ensure_space(kv, size)))
        return rc;

    rec_hdr_t hdr = { REC_MAGIC, (uint8_t)klen, flags, (uint16_t)vlen, 0xFFFFu, 0, 0 };
    hdr.data_crc = pfkv_crc32(pfkv_crc32(0, key, klen), val, vlen);
    hdr.hdr_crc = pfkv_crc32(0, &hdr, 12);

    /* Stream header, key, value and padding through a small buffer. */
    const uint32_t addr = sect_addr(kv, kv->active) + kv->write_off;
    const uint8_t *parts[3] = { (const uint8_t *)&hdr, (const uint8_t *)key, (const uint8_t *)val };
    const uint32_t lens[3] = { REC_HDR_SIZE, (uint32_t)klen, (uint32_t)vlen };
    uint8_t buf[CHUNK];
    uint32_t fill = 0, dst = addr, body = size - p;
    for (int part = 0; part < 3; part++) {
        for (uint32_t j = 0; j < lens[part]; j++) {
            buf[fill++] = parts[part][j];
            if (fill == CHUNK) {
                if ((rc = f_prog(kv, dst, buf, CHUNK)))
                    return rc;
                dst += CHUNK;
                fill = 0;
            }
        }
    }
    if (fill) {
        memset(buf + fill, 0xFF, CHUNK - fill);
        if ((rc = f_prog(kv, dst, buf, addr + body - dst)))
            return rc;
    }
    /* Commit: the record exists only once this unit is fully programmed. */
    fill_commit(buf, p);
    if ((rc = f_prog(kv, addr + body, buf, p)))
        return rc;
    kv->write_off += size;
    kv->stats.user_bytes += klen + vlen;
    return index_set(kv, key, klen, addr, size, flags == FLAG_TOMB);
}

int pfkv_put(pfkv_t *kv, const char *key, const void *val, size_t len)
{
    size_t klen;
    if (!kv || !kv->mounted || (!val && len))
        return PFKV_ERR_INVAL;
    if (check_key(key, &klen))
        return PFKV_ERR_INVAL;
    if (len > pfkv_max_value_len(kv, klen))
        return PFKV_ERR_TOOBIG;
    return write_record(kv, key, klen, val ? val : "", len, FLAG_VALUE);
}

int pfkv_delete(pfkv_t *kv, const char *key)
{
    size_t klen;
    int i, rc;
    if (!kv || !kv->mounted || check_key(key, &klen))
        return PFKV_ERR_INVAL;
    if ((rc = find_slot(kv, key, klen, hash_key(key, klen), &i)))
        return rc;
    if (i < 0 || (kv->slots[i].addr & TOMB_BIT))
        return PFKV_ERR_NOT_FOUND;
    return write_record(kv, key, klen, "", 0, FLAG_TOMB);
}

int pfkv_get(pfkv_t *kv, const char *key, void *buf, size_t buflen, size_t *out_len)
{
    size_t klen;
    int i, rc;
    if (!kv || !kv->mounted || check_key(key, &klen) || (!buf && buflen))
        return PFKV_ERR_INVAL;
    if ((rc = find_slot(kv, key, klen, hash_key(key, klen), &i)))
        return rc;
    if (i < 0 || (kv->slots[i].addr & TOMB_BIT))
        return PFKV_ERR_NOT_FOUND;
    uint32_t addr = kv->slots[i].addr;
    rec_hdr_t h;
    if ((rc = f_read(kv, addr, &h, REC_HDR_SIZE)))
        return rc;
    if (out_len)
        *out_len = h.val_len;
    if (h.val_len > buflen)
        return PFKV_ERR_TOOBIG;
    if (h.val_len && (rc = f_read(kv, addr + REC_HDR_SIZE + h.key_len, buf, h.val_len)))
        return rc;
    /* Re-verify the payload: catches bit rot or a misbehaving driver. */
    if (pfkv_crc32(pfkv_crc32(0, key, klen), buf, h.val_len) != h.data_crc)
        return PFKV_ERR_CORRUPT;
    return PFKV_OK;
}

int pfkv_iterate(pfkv_t *kv, pfkv_iter_cb cb, void *arg)
{
    if (!kv || !kv->mounted || !cb)
        return PFKV_ERR_INVAL;
    for (uint32_t i = 0; i < kv->nkeys; i++) {
        if (kv->slots[i].addr & TOMB_BIT)
            continue;
        rec_hdr_t h;
        char key[PFKV_MAX_KEY_LEN + 1];
        int rc = f_read(kv, kv->slots[i].addr, &h, REC_HDR_SIZE);
        if (rc || (rc = f_read(kv, kv->slots[i].addr + REC_HDR_SIZE, key, h.key_len)))
            return rc;
        key[h.key_len] = '\0';
        if (cb(key, h.val_len, arg))
            break;
    }
    return PFKV_OK;
}

uint32_t pfkv_count(const pfkv_t *kv)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < kv->nkeys; i++)
        n += !(kv->slots[i].addr & TOMB_BIT);
    return n;
}
