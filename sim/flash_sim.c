#include "flash_sim.h"

#include <string.h>

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

void flash_sim_init(flash_sim_t *f, uint8_t *mem, uint8_t *unit_programmed, uint32_t *erase_counts,
                    uint32_t sector_size, uint32_t sector_count, uint32_t prog_size)
{
    memset(f, 0, sizeof *f);
    f->mem = mem;
    f->unit_programmed = unit_programmed;
    f->erase_counts = erase_counts;
    f->sector_size = sector_size;
    f->sector_count = sector_count;
    f->prog_size = prog_size;
    f->cut_at = FLASH_SIM_NO_CUT;
    memset(mem, 0xFF, (size_t)sector_size * sector_count);
    memset(unit_programmed, 0, (size_t)sector_size * sector_count / prog_size);
    memset(erase_counts, 0, sizeof(uint32_t) * sector_count);
}

void flash_sim_power_on(flash_sim_t *f)
{
    f->dead = 0;
    f->ops = 0;
    f->cut_at = FLASH_SIM_NO_CUT;
}

void flash_sim_arm_cut(flash_sim_t *f, uint64_t n, flash_cut_mode_t mode, uint32_t seed)
{
    f->cut_at = f->ops + n;
    f->cut_mode = mode;
    f->rng = seed;
}

static int power_lost(flash_sim_t *f)
{
    f->dead = 1;
    if (f->on_cut)
        f->on_cut(f->cut_arg);
    return -1;
}

static int sim_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    flash_sim_t *f = (flash_sim_t *)ctx;
    if (f->dead || (uint64_t)addr + len > (uint64_t)f->sector_size * f->sector_count)
        return -1;
    memcpy(buf, f->mem + addr, len);
    return 0;
}

static int sim_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len)
{
    flash_sim_t *f = (flash_sim_t *)ctx;
    const uint8_t *src = (const uint8_t *)buf;
    const uint32_t p = f->prog_size;
    if (f->dead)
        return -1;
    if (addr % p || len % p || len == 0 || (uint64_t)addr + len > (uint64_t)f->sector_size * f->sector_count) {
        f->violations++;
        return -1;
    }
    for (uint32_t u = 0; u < len; u += p) {
        if (!f->unit_programmed[(addr + u) / p])
            continue;
        for (uint32_t i = 0; i < p; i++) {
            if (src[u + i] != 0) {
                f->violations++;
                return -1;
            }
        }
    }
    if (f->ops++ == f->cut_at) {
        if (f->cut_mode == CUT_TORN) {
            uint32_t k = xorshift(&f->rng) % len;
            for (uint32_t i = 0; i < k; i++)
                f->mem[addr + i] &= src[i];
            /* Byte k: only a random subset of the bits to clear made it. */
            f->mem[addr + k] &= (uint8_t)(src[k] | (uint8_t)xorshift(&f->rng));
            for (uint32_t u = 0; u <= k; u += p)
                f->unit_programmed[(addr + u) / p] = 1;
        }
        return power_lost(f);
    }
    for (uint32_t i = 0; i < len; i++)
        f->mem[addr + i] &= src[i];
    for (uint32_t u = 0; u < len; u += p)
        f->unit_programmed[(addr + u) / p] = 1;
    f->prog_bytes += len;
    return 0;
}

static int sim_erase(void *ctx, uint32_t sector)
{
    flash_sim_t *f = (flash_sim_t *)ctx;
    const uint32_t base = sector * f->sector_size, p = f->prog_size;
    if (f->dead)
        return -1;
    if (sector >= f->sector_count) {
        f->violations++;
        return -1;
    }
    f->erase_counts[sector]++;
    f->erases++;
    if (f->ops++ == f->cut_at) {
        if (f->cut_mode == CUT_TORN) {
            /* Interrupted erase: a prefix is erased, one byte is partially
             * erased, the rest keeps its old contents. */
            uint32_t k = xorshift(&f->rng) % f->sector_size;
            memset(f->mem + base, 0xFF, k);
            f->mem[base + k] |= (uint8_t)xorshift(&f->rng);
            for (uint32_t u = 0; u + p <= k; u += p)
                f->unit_programmed[(base + u) / p] = 0;
        }
        return power_lost(f);
    }
    memset(f->mem + base, 0xFF, f->sector_size);
    memset(f->unit_programmed + base / p, 0, f->sector_size / p);
    return 0;
}

pfkv_flash_t flash_sim_hal(flash_sim_t *f)
{
    pfkv_flash_t h;
    h.sector_size = f->sector_size;
    h.sector_count = f->sector_count;
    h.prog_size = f->prog_size;
    h.ctx = f;
    h.read = sim_read;
    h.prog = sim_prog;
    h.erase = sim_erase;
    return h;
}
