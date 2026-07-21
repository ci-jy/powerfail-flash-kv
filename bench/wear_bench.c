/*
 * Wear-leveling benchmark: the same device workload (static calibration
 * data, a few hot counters, occasional settings changes) against
 *   - a naive in-place store (read-modify-erase-write of a home sector),
 *   - pfkv with round-robin GC,
 *   - pfkv with greedy GC and no static wear leveling,
 *   - pfkv with greedy GC plus static wear leveling.
 * Reports erase-count spread and write amplification after a fixed number of
 * writes, and the number of writes until the first sector reaches its rated
 * endurance. Prints JSON on stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_sim.h"
#include "naive_store.h"
#include "pfkv.h"

#define SECTOR_SIZE 4096u
#define SECTORS 8u
#define PROG 8u
#define ENDURANCE 10000u
#define FIXED_WRITES 200000u
#define STATIC_KEYS 96
#define MAX_LIFETIME_WRITES 60000000ull

typedef enum { NAIVE, LOG_OLDEST, LOG_GREEDY, LOG_GREEDY_WL } variant_t;
static const char *variant_names[] = { "naive in-place", "pfkv round-robin GC", "pfkv greedy GC",
                                       "pfkv greedy GC + static WL" };

static uint8_t mem[SECTOR_SIZE * SECTORS];
static uint8_t units[SECTOR_SIZE * SECTORS / PROG];
static uint32_t ecs[SECTORS];
static flash_sim_t sim;
static pfkv_flash_t hal;
static pfkv_t kv;
static naive_store_t naive;

typedef struct {
    uint32_t rng;
    uint32_t counters[4];
    uint64_t n;
} gen_t;

static uint32_t rnd(gen_t *g)
{
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return g->rng;
}

static int do_put(variant_t v, const char *key, const void *val, size_t len)
{
    return v == NAIVE ? naive_put(&naive, key, val, len) : pfkv_put(&kv, key, val, len);
}

static uint64_t user_bytes;

static int put_counted(variant_t v, const char *key, const void *val, size_t len)
{
    user_bytes += strlen(key) + len;
    return do_put(v, key, val, len);
}

/* One step of the device workload: mostly hot counters, sometimes settings. */
static int step(variant_t v, gen_t *g)
{
    char key[24];
    g->n++;
    if (g->n % 50 == 0) {
        uint8_t val[24];
        for (int i = 0; i < 24; i++)
            val[i] = (uint8_t)rnd(g);
        snprintf(key, sizeof key, "cfg/opt%u", rnd(g) % 8);
        return put_counted(v, key, val, sizeof val);
    }
    uint32_t r = rnd(g) % 16, k = r < 10 ? 0 : r < 14 ? 1 : r < 15 ? 2 : 3;
    g->counters[k]++;
    snprintf(key, sizeof key, "cnt/%u", k);
    return put_counted(v, key, &g->counters[k], 4);
}

static int setup(variant_t v, gen_t *g)
{
    flash_sim_init(&sim, mem, units, ecs, SECTOR_SIZE, SECTORS, PROG);
    hal = flash_sim_hal(&sim);
    memset(g, 0, sizeof *g);
    g->rng = 0xBE11C4u;
    user_bytes = 0;
    if (v == NAIVE) {
        naive_init(&naive, &hal);
    } else {
        pfkv_config_t cfg = { v == LOG_OLDEST ? PFKV_GC_OLDEST : PFKV_GC_GREEDY,
                              v == LOG_GREEDY_WL ? 32u : 0u };
        if (pfkv_mount(&kv, &hal, &cfg))
            return -1;
    }
    /* Factory calibration: written once, never changed. */
    for (int i = 0; i < STATIC_KEYS; i++) {
        char key[24];
        uint8_t val[32];
        snprintf(key, sizeof key, "cal/ch%02d", i);
        for (int j = 0; j < 32; j++)
            val[j] = (uint8_t)rnd(g);
        if (put_counted(v, key, val, sizeof val))
            return -1;
    }
    return 0;
}

static uint32_t max_ec(void)
{
    uint32_t m = 0;
    for (uint32_t s = 0; s < SECTORS; s++)
        m = ecs[s] > m ? ecs[s] : m;
    return m;
}

int main(void)
{
    printf("{\n  \"geometry\": {\"sector_size\": %u, \"sectors\": %u, \"prog_size\": %u, \"endurance\": %u},\n",
           SECTOR_SIZE, SECTORS, PROG, ENDURANCE);
    printf("  \"fixed_writes\": %u,\n  \"variants\": [\n", FIXED_WRITES);
    for (int v = NAIVE; v <= LOG_GREEDY_WL; v++) {
        gen_t g;
        if (setup((variant_t)v, &g)) {
            fprintf(stderr, "setup failed\n");
            return 1;
        }
        for (uint32_t i = 0; i < FIXED_WRITES; i++)
            if (step((variant_t)v, &g)) {
                fprintf(stderr, "%s: write failed\n", variant_names[v]);
                return 1;
            }
        uint32_t mn = UINT32_MAX, mx = 0;
        for (uint32_t s = 0; s < SECTORS; s++) {
            mn = ecs[s] < mn ? ecs[s] : mn;
            mx = ecs[s] > mx ? ecs[s] : mx;
        }
        double wa = (double)sim.prog_bytes / (double)user_bytes;
        printf("    {\"name\": \"%s\", \"erase_counts\": [", variant_names[v]);
        for (uint32_t s = 0; s < SECTORS; s++)
            printf("%u%s", ecs[s], s + 1 < SECTORS ? ", " : "");
        printf("], \"erase_min\": %u, \"erase_max\": %u, \"erase_spread\": %u, \"total_erases\": %llu, "
               "\"write_amplification\": %.2f, ",
               mn, mx, mx - mn, (unsigned long long)sim.erases, wa);

        /* Lifetime: keep going until a sector reaches rated endurance. */
        setup((variant_t)v, &g);
        uint64_t writes = 0;
        while (max_ec() < ENDURANCE && writes < MAX_LIFETIME_WRITES) {
            if (step((variant_t)v, &g)) {
                fprintf(stderr, "%s: write failed\n", variant_names[v]);
                return 1;
            }
            writes++;
        }
        uint64_t total = 0;
        for (uint32_t s = 0; s < SECTORS; s++)
            total += ecs[s];
        printf("\"writes_to_first_wearout\": %llu, \"wear_utilisation\": %.3f}%s\n", (unsigned long long)writes,
               (double)total / ((double)ENDURANCE * SECTORS), v < LOG_GREEDY_WL ? "," : "");
        fprintf(stderr, "%-28s spread=%-6u WA=%-7.2f writes_to_wearout=%llu\n", variant_names[v], mx - mn, wa,
                (unsigned long long)writes);
    }
    printf("  ]\n}\n");
    return 0;
}
