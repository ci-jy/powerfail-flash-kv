/*
 * Exhaustive power-cut harness.
 *
 * For each workload the harness first counts the flash operations T of an
 * uninterrupted run. It then replays the workload T times per cut mode, losing
 * power at operation 0, 1, ..., T-1 (clean cut, then torn write/erase). After
 * every cut it remounts and checks:
 *   - mount succeeds and the flash simulator saw no rule violations;
 *   - every acknowledged write is present with its exact value;
 *   - the operation in flight when power failed is either fully applied or
 *     not at all (never half-visible), and no other key changed;
 *   - the store accepts and persists a new write after recovery.
 * If the recovery mount itself performs flash operations (repairing an
 * interrupted GC), power is additionally cut at each of those operations.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_sim.h"
#include "pfkv.h"

#define MAX_KEYS 64
#define MAX_VAL 320

typedef struct {
    int del;
    uint8_t key;
    uint16_t len;
    uint8_t val[MAX_VAL];
} op_t;

typedef struct {
    const char *name;
    uint32_t sector_size, sector_count, prog_size;
    int nkeys;
    char keys[MAX_KEYS][PFKV_MAX_KEY_LEN + 1];
    int nops;
    op_t *ops;
} workload_t;

typedef struct {
    int present;
    uint16_t len;
    uint8_t val[MAX_VAL];
} entry_t;

typedef struct {
    uint64_t cuts, nested_cuts, lost_acked, half_visible, mount_failures, inconsistent, violations;
    uint64_t reported;
} result_t;

static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static uint32_t rnd_range(uint32_t lo, uint32_t hi) { return lo + rnd() % (hi - lo + 1); }

/* ------------------------------------------------------------ workloads */

static void fill_value(op_t *op, uint32_t len)
{
    op->len = (uint16_t)len;
    for (uint32_t i = 0; i < len; i++)
        op->val[i] = (uint8_t)rnd();
}

/* Configuration written in bursts: a handful of settings change together,
 * occasionally one is removed. */
static void gen_config_burst(workload_t *w, int nops)
{
    static const char *names[] = { "wifi/ssid", "wifi/psk", "net/ip", "net/mask", "net/gw", "mqtt/host",
                                   "mqtt/port", "mqtt/topic", "log/level", "ui/brightness", "ui/lang",
                                   "cal/adc0", "cal/adc1", "cal/temp", "dev/name", "dev/tz" };
    rng_state = 0xC0F16u;
    w->name = "config-burst";
    w->sector_size = 1024;
    w->sector_count = 4;
    w->prog_size = 8;
    w->nkeys = 16;
    for (int i = 0; i < w->nkeys; i++)
        strcpy(w->keys[i], names[i]);
    w->ops = calloc((size_t)nops, sizeof(op_t));
    int n = 0;
    while (n < nops) {
        int burst = (int)rnd_range(1, 8);
        for (int b = 0; b < burst && n < nops; b++, n++) {
            op_t *op = &w->ops[n];
            op->key = (uint8_t)(rnd() % (uint32_t)w->nkeys);
            op->del = rnd() % 20 == 0;
            if (!op->del)
                fill_value(op, rnd_range(4, 48));
        }
    }
    w->nops = nops;
}

/* Monotonic counters (boot count, runtime hours, event totals). */
static void gen_counters(workload_t *w, int nops)
{
    rng_state = 0xC0C0A1u;
    w->name = "counters";
    w->sector_size = 512;
    w->sector_count = 4;
    w->prog_size = 16;
    w->nkeys = 4;
    strcpy(w->keys[0], "boot_count");
    strcpy(w->keys[1], "uptime_h");
    strcpy(w->keys[2], "events");
    strcpy(w->keys[3], "faults");
    w->ops = calloc((size_t)nops, sizeof(op_t));
    uint32_t c[4] = { 0 };
    for (int n = 0; n < nops; n++) {
        op_t *op = &w->ops[n];
        uint32_t k = rnd() % 8;
        k = k < 4 ? 2 : k < 6 ? 1 : k < 7 ? 0 : 3; /* skewed update rates */
        op->key = (uint8_t)k;
        c[k]++;
        op->len = 4;
        memcpy(op->val, &c[k], 4);
    }
    w->nops = nops;
}

/* Keys of 1..48 bytes, values of 0..300 bytes, with deletes. */
static void gen_mixed(workload_t *w, int nops)
{
    rng_state = 0x313Du;
    w->name = "mixed-sizes";
    w->sector_size = 2048;
    w->sector_count = 6;
    w->prog_size = 4;
    w->nkeys = 24;
    for (int i = 0; i < w->nkeys; i++) {
        uint32_t len = rnd_range(1, 48);
        for (uint32_t j = 0; j < len; j++)
            w->keys[i][j] = (char)('a' + rnd() % 26);
        w->keys[i][len] = 0;
        w->keys[i][0] = (char)('A' + i); /* keep keys distinct */
    }
    w->ops = calloc((size_t)nops, sizeof(op_t));
    for (int n = 0; n < nops; n++) {
        op_t *op = &w->ops[n];
        op->key = (uint8_t)(rnd() % (uint32_t)w->nkeys);
        op->del = rnd() % 10 == 0;
        if (!op->del)
            fill_value(op, rnd() % 4 == 0 ? rnd_range(100, 300) : rnd_range(0, 40));
    }
    w->nops = nops;
}

/* --------------------------------------------------------------- engine */

static flash_sim_t sim;
static pfkv_flash_t hal;
static uint8_t *mem, *units, *snap_mem, *snap_units;
static uint32_t ecs[PFKV_MAX_SECTORS], snap_ecs[PFKV_MAX_SECTORS];
static jmp_buf cut_jmp;
static pfkv_t kv;
static entry_t model[MAX_KEYS];
static int pending = -1;
static entry_t pending_new;

static void on_cut(void *arg)
{
    (void)arg;
    longjmp(cut_jmp, 1);
}

static void setup(const workload_t *w)
{
    size_t size = (size_t)w->sector_size * w->sector_count;
    mem = realloc(mem, size);
    units = realloc(units, size / w->prog_size);
    snap_mem = realloc(snap_mem, size);
    snap_units = realloc(snap_units, size / w->prog_size);
    flash_sim_init(&sim, mem, units, ecs, w->sector_size, w->sector_count, w->prog_size);
    sim.on_cut = on_cut;
    hal = flash_sim_hal(&sim);
}

static void snapshot(const workload_t *w, int save)
{
    size_t size = (size_t)w->sector_size * w->sector_count;
    if (save) {
        memcpy(snap_mem, mem, size);
        memcpy(snap_units, units, size / w->prog_size);
        memcpy(snap_ecs, ecs, sizeof ecs);
    } else {
        memcpy(mem, snap_mem, size);
        memcpy(units, snap_units, size / w->prog_size);
        memcpy(ecs, snap_ecs, sizeof ecs);
    }
}

/* Run the workload from blank flash. Returns 1 if power was cut. */
static int run_workload(const workload_t *w, uint64_t cut_at, flash_cut_mode_t mode, uint32_t seed)
{
    flash_sim_init(&sim, mem, units, ecs, w->sector_size, w->sector_count, w->prog_size);
    sim.on_cut = on_cut;
    memset(model, 0, sizeof model);
    pending = -1;
    if (cut_at != FLASH_SIM_NO_CUT)
        flash_sim_arm_cut(&sim, cut_at, mode, seed);
    if (setjmp(cut_jmp))
        return 1;
    if (pfkv_mount(&kv, &hal, NULL) != PFKV_OK) {
        fprintf(stderr, "initial mount failed\n");
        exit(2);
    }
    for (int i = 0; i < w->nops; i++) {
        const op_t *op = &w->ops[i];
        pending = op->key;
        pending_new.present = !op->del;
        pending_new.len = op->len;
        memcpy(pending_new.val, op->val, op->len);
        int rc = op->del ? pfkv_delete(&kv, w->keys[op->key])
                         : pfkv_put(&kv, w->keys[op->key], op->val, op->len);
        if (rc == PFKV_OK)
            model[op->key] = pending_new;
        else if (!(op->del && rc == PFKV_ERR_NOT_FOUND) && rc != PFKV_ERR_NOSPC) {
            fprintf(stderr, "%s: op %d failed without power loss: %d\n", w->name, i, rc);
            exit(2);
        }
        pending = -1;
    }
    return 0;
}

static int entry_matches(const entry_t *e, int rc, const uint8_t *buf, size_t len)
{
    if (!e->present)
        return rc == PFKV_ERR_NOT_FOUND;
    return rc == PFKV_OK && len == e->len && memcmp(buf, e->val, len) == 0;
}

static int count_cb(const char *key, size_t len, void *arg)
{
    (void)key;
    (void)len;
    (*(uint32_t *)arg)++;
    return 0;
}

/* Remount after a cut and check the store against the model. */
static void verify(const workload_t *w, result_t *r)
{
    uint8_t buf[MAX_VAL];
    if (pfkv_mount(&kv, &hal, NULL) != PFKV_OK) {
        r->mount_failures++;
        return;
    }
    uint32_t expect_count = 0, seen = 0;
    int bad = 0;
    for (int k = 0; k < w->nkeys; k++) {
        size_t len = 0;
        int rc = pfkv_get(&kv, w->keys[k], buf, sizeof buf, &len);
        int ok_old = entry_matches(&model[k], rc, buf, len);
        int ok_new = k == pending && entry_matches(&pending_new, rc, buf, len);
        if (ok_old || ok_new) {
            expect_count += rc == PFKV_OK;
            continue;
        }
        bad = 1;
        if (k == pending || rc == PFKV_ERR_CORRUPT)
            r->half_visible++;
        else
            r->lost_acked++;
    }
    pfkv_iterate(&kv, count_cb, &seen);
    if (!bad && seen != expect_count)
        r->inconsistent++;

    /* The recovered store must still accept and persist writes. */
    static const char probe[] = "probe-after-recovery";
    if (pfkv_put(&kv, "__probe", probe, sizeof probe) != PFKV_OK ||
        pfkv_mount(&kv, &hal, NULL) != PFKV_OK ||
        pfkv_get(&kv, "__probe", buf, sizeof buf, NULL) != PFKV_OK || memcmp(buf, probe, sizeof probe))
        r->inconsistent++;
}

static void run_cut(const workload_t *w, uint64_t n, flash_cut_mode_t mode, result_t *r)
{
    uint32_t seed = (uint32_t)(n * 2654435761u) ^ (uint32_t)mode ^ 0x5EEDu;
    if (!run_workload(w, n, mode, seed))
        return; /* cut point beyond the end of the run */
    r->cuts++;
    flash_sim_power_on(&sim);

    /* Count the operations the recovery mount performs, then cut at each. */
    snapshot(w, 1);
    int save_pending = pending;
    entry_t save_model[MAX_KEYS];
    memcpy(save_model, model, sizeof model);
    pfkv_t probe;
    pfkv_mount(&probe, &hal, NULL);
    uint64_t mount_ops = sim.ops;
    for (uint64_t m = 0; m < mount_ops; m++) {
        snapshot(w, 0);
        flash_sim_power_on(&sim);
        flash_sim_arm_cut(&sim, m, mode, seed + (uint32_t)m + 1u);
        if (!setjmp(cut_jmp))
            pfkv_mount(&probe, &hal, NULL);
        r->nested_cuts++;
        flash_sim_power_on(&sim);
        if (setjmp(cut_jmp)) {
            fprintf(stderr, "unexpected cut\n");
            exit(2);
        }
        verify(w, r);
        memcpy(model, save_model, sizeof model);
        pending = save_pending;
    }
    snapshot(w, 0);
    flash_sim_power_on(&sim);
    if (setjmp(cut_jmp)) {
        fprintf(stderr, "unexpected cut\n");
        exit(2);
    }
    verify(w, r);
    r->violations += sim.violations;
    uint64_t fails = r->lost_acked + r->half_visible + r->mount_failures + r->inconsistent + r->violations;
    if (fails != r->reported) {
        fprintf(stderr, "%s: failure after cut at op %llu (%s)\n", w->name, (unsigned long long)n,
                mode == CUT_TORN ? "torn" : "clean");
        r->reported = fails;
    }
}

/* The uninterrupted run must also survive a remount. */
static void verify_uncut(const workload_t *w, result_t *r)
{
    flash_sim_power_on(&sim);
    if (setjmp(cut_jmp))
        exit(2);
    verify(w, r);
}

int main(int argc, char **argv)
{
    const char *json = NULL;
    double scale = 1.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json") && i + 1 < argc)
            json = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = atof(argv[++i]);
    }
    static workload_t wl[3];
    gen_config_burst(&wl[0], (int)(1400 * scale));
    gen_counters(&wl[1], (int)(3000 * scale));
    gen_mixed(&wl[2], (int)(1000 * scale));

    FILE *jf = json ? fopen(json, "w") : NULL;
    if (jf)
        fprintf(jf, "{\n  \"workloads\": [\n");
    printf("%-13s %8s %10s %8s %8s %11s %8s %13s %10s\n", "workload", "ops", "flash_ops", "cuts",
           "nested", "lost_acked", "half", "bad_mounts", "violations");
    uint64_t total_cuts = 0, total_fail = 0;
    for (int i = 0; i < 3; i++) {
        workload_t *w = &wl[i];
        setup(w);
        run_workload(w, FLASH_SIM_NO_CUT, CUT_CLEAN, 0);
        uint64_t total = sim.ops;
        result_t r = { 0 };
        verify_uncut(w, &r);
        for (int mode = CUT_CLEAN; mode <= CUT_TORN; mode++)
            for (uint64_t n = 0; n < total; n++)
                run_cut(w, n, (flash_cut_mode_t)mode, &r);
        uint64_t fail = r.lost_acked + r.half_visible + r.mount_failures + r.inconsistent + r.violations;
        total_cuts += r.cuts + r.nested_cuts;
        total_fail += fail;
        printf("%-13s %8d %10llu %8llu %8llu %11llu %8llu %13llu %10llu\n", w->name, w->nops,
               (unsigned long long)total, (unsigned long long)r.cuts, (unsigned long long)r.nested_cuts,
               (unsigned long long)r.lost_acked, (unsigned long long)r.half_visible,
               (unsigned long long)(r.mount_failures + r.inconsistent), (unsigned long long)r.violations);
        if (jf)
            fprintf(jf,
                    "    {\"name\": \"%s\", \"ops\": %d, \"flash_ops\": %llu, \"sector_size\": %u, "
                    "\"sector_count\": %u, \"prog_size\": %u, \"cuts\": %llu, \"nested_cuts\": %llu, "
                    "\"lost_acked\": %llu, \"half_visible\": %llu, \"bad_mounts\": %llu, \"violations\": %llu}%s\n",
                    w->name, w->nops, (unsigned long long)total, w->sector_size, w->sector_count, w->prog_size,
                    (unsigned long long)r.cuts, (unsigned long long)r.nested_cuts,
                    (unsigned long long)r.lost_acked, (unsigned long long)r.half_visible,
                    (unsigned long long)(r.mount_failures + r.inconsistent), (unsigned long long)r.violations,
                    i < 2 ? "," : "");
        free(w->ops);
    }
    if (jf) {
        fprintf(jf, "  ],\n  \"total_cuts\": %llu,\n  \"total_failures\": %llu\n}\n",
                (unsigned long long)total_cuts, (unsigned long long)total_fail);
        fclose(jf);
    }
    printf("\n%llu injected power cuts, %llu failures: %s\n", (unsigned long long)total_cuts,
           (unsigned long long)total_fail, total_fail ? "FAIL" : "PASS");
    return total_fail ? 1 : 0;
}
