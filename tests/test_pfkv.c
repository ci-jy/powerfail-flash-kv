#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_sim.h"
#include "pfkv.h"
#include "unity.h"

#define SS 1024u
#define SC 6u
#define PS 8u

static uint8_t mem[SS * SC];
static uint8_t units[SS * SC / 4];
static uint32_t ecs[SC];
static flash_sim_t sim;
static pfkv_flash_t hal;
static pfkv_t kv;

static void setup_geometry(uint32_t ss, uint32_t sc, uint32_t ps)
{
    flash_sim_init(&sim, mem, units, ecs, ss, sc, ps);
    hal = flash_sim_hal(&sim);
}

void setUp(void)
{
    setup_geometry(SS, SC, PS);
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_mount(&kv, &hal, NULL));
}

void tearDown(void) { TEST_ASSERT_EQUAL_UINT32(0, sim.violations); }

static void remount(void) { TEST_ASSERT_EQUAL(PFKV_OK, pfkv_mount(&kv, &hal, NULL)); }

static void assert_value(const char *key, const char *expect)
{
    char buf[256];
    size_t n = 0;
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_get(&kv, key, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_size_t(strlen(expect), n);
    TEST_ASSERT_EQUAL_MEMORY(expect, buf, n);
}

/* ------------------------------------------------------------ simulator */

static void test_sim_program_only_clears_bits(void)
{
    uint8_t a[8] = { 0x0F, 0, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL(0, hal.prog(hal.ctx, 2 * SS, a, 8));
    TEST_ASSERT_EQUAL_HEX8(0x0F, mem[2 * SS]);
    uint8_t b[8] = { 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    /* Re-programming a unit with non-zero data is a violation. */
    TEST_ASSERT_NOT_EQUAL(0, hal.prog(hal.ctx, 2 * SS, b, 8));
    TEST_ASSERT_EQUAL_UINT32(1, sim.violations);
    sim.violations = 0;
    uint8_t z[8] = { 0 };
    TEST_ASSERT_EQUAL(0, hal.prog(hal.ctx, 2 * SS, z, 8));
    TEST_ASSERT_EQUAL_HEX8(0x00, mem[2 * SS]);
    TEST_ASSERT_EQUAL(0, hal.erase(hal.ctx, 2));
    TEST_ASSERT_EQUAL_HEX8(0xFF, mem[2 * SS]);
}

static void test_sim_rejects_misaligned_program(void)
{
    uint8_t a[8] = { 0 };
    TEST_ASSERT_NOT_EQUAL(0, hal.prog(hal.ctx, 2 * SS + 4, a, 8));
    TEST_ASSERT_NOT_EQUAL(0, hal.prog(hal.ctx, 2 * SS, a, 6));
    TEST_ASSERT_EQUAL_UINT32(2, sim.violations);
    sim.violations = 0;
}

static void test_sim_cut_kills_device(void)
{
    uint8_t a[8] = { 0 };
    flash_sim_arm_cut(&sim, 0, CUT_CLEAN, 1);
    TEST_ASSERT_NOT_EQUAL(0, hal.prog(hal.ctx, 2 * SS, a, 8));
    TEST_ASSERT_EQUAL_HEX8(0xFF, mem[2 * SS]);
    TEST_ASSERT_NOT_EQUAL(0, hal.read(hal.ctx, 0, a, 8));
    flash_sim_power_on(&sim);
    TEST_ASSERT_EQUAL(0, hal.read(hal.ctx, 0, a, 8));
}

static void test_sim_torn_program_is_partial(void)
{
    uint8_t a[64];
    memset(a, 0, sizeof a);
    flash_sim_arm_cut(&sim, 0, CUT_TORN, 12345);
    TEST_ASSERT_NOT_EQUAL(0, hal.prog(hal.ctx, 3 * SS, a, 64));
    int zeros = 0;
    for (int i = 0; i < 64; i++)
        zeros += mem[3 * SS + i] == 0;
    TEST_ASSERT_TRUE(zeros < 64);
    TEST_ASSERT_EQUAL_HEX8(0xFF, mem[3 * SS + 63]);
    flash_sim_power_on(&sim);
}

/* ---------------------------------------------------------------- store */

static void test_put_get_roundtrip(void)
{
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "wifi.ssid", "lab-net", 7));
    assert_value("wifi.ssid", "lab-net");
    TEST_ASSERT_EQUAL_UINT32(1, pfkv_count(&kv));
}

static void test_get_missing_key(void)
{
    char buf[4];
    TEST_ASSERT_EQUAL(PFKV_ERR_NOT_FOUND, pfkv_get(&kv, "nope", buf, sizeof buf, NULL));
}

static void test_overwrite_keeps_latest(void)
{
    char v[16];
    for (int i = 0; i < 50; i++) {
        snprintf(v, sizeof v, "value-%d", i);
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "k", v, strlen(v)));
    }
    assert_value("k", "value-49");
    remount();
    assert_value("k", "value-49");
    TEST_ASSERT_EQUAL_UINT32(1, pfkv_count(&kv));
}

static void test_delete_and_persist(void)
{
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "a", "1", 1));
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "b", "2", 1));
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_delete(&kv, "a"));
    TEST_ASSERT_EQUAL(PFKV_ERR_NOT_FOUND, pfkv_delete(&kv, "a"));
    remount();
    char buf[4];
    TEST_ASSERT_EQUAL(PFKV_ERR_NOT_FOUND, pfkv_get(&kv, "a", buf, sizeof buf, NULL));
    assert_value("b", "2");
    TEST_ASSERT_EQUAL_UINT32(1, pfkv_count(&kv));
}

static void test_empty_value(void)
{
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "flag", NULL, 0));
    size_t n = 99;
    char buf[1];
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_get(&kv, "flag", buf, 0, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

static void test_buffer_too_small(void)
{
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "k", "0123456789", 10));
    char buf[4];
    size_t n = 0;
    TEST_ASSERT_EQUAL(PFKV_ERR_TOOBIG, pfkv_get(&kv, "k", buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_size_t(10, n);
}

static void test_invalid_arguments(void)
{
    char longkey[PFKV_MAX_KEY_LEN + 2];
    memset(longkey, 'x', sizeof longkey - 1);
    longkey[sizeof longkey - 1] = 0;
    TEST_ASSERT_EQUAL(PFKV_ERR_INVAL, pfkv_put(&kv, "", "v", 1));
    TEST_ASSERT_EQUAL(PFKV_ERR_INVAL, pfkv_put(&kv, longkey, "v", 1));
    TEST_ASSERT_EQUAL(PFKV_ERR_INVAL, pfkv_put(&kv, NULL, "v", 1));
    static uint8_t big[2048];
    TEST_ASSERT_EQUAL(PFKV_ERR_TOOBIG, pfkv_put(&kv, "big", big, sizeof big));
    size_t max = pfkv_max_value_len(&kv, 3);
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "big", big, max));
}

static void test_bad_geometry_rejected(void)
{
    pfkv_flash_t bad = hal;
    bad.prog_size = 6;
    TEST_ASSERT_EQUAL(PFKV_ERR_INVAL, pfkv_mount(&kv, &bad, NULL));
    bad = hal;
    bad.sector_count = 2;
    TEST_ASSERT_EQUAL(PFKV_ERR_INVAL, pfkv_mount(&kv, &bad, NULL));
}

static void test_gc_reclaims_space_over_many_updates(void)
{
    char v[40];
    for (int i = 0; i < 3000; i++) {
        snprintf(v, sizeof v, "counter=%08d", i);
        char key[8];
        snprintf(key, sizeof key, "c%d", i % 5);
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, key, v, strlen(v)));
    }
    TEST_ASSERT_TRUE(kv.stats.gc_runs > 0);
    remount();
    assert_value("c4", "counter=00002999");
    assert_value("c0", "counter=00002995");
    TEST_ASSERT_EQUAL_UINT32(5, pfkv_count(&kv));
}

static int count_cb(const char *key, size_t len, void *arg)
{
    (void)key;
    (void)len;
    (*(int *)arg)++;
    return 0;
}

static void test_iterate_visits_live_keys(void)
{
    char key[16];
    for (int i = 0; i < 20; i++) {
        snprintf(key, sizeof key, "key%02d", i);
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, key, key, strlen(key)));
    }
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_delete(&kv, "key03"));
    int n = 0;
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_iterate(&kv, count_cb, &n));
    TEST_ASSERT_EQUAL(19, n);
}

static void test_capacity_limit_returns_nospc(void)
{
    static uint8_t v[400];
    char key[16];
    int rc = PFKV_OK, i;
    for (i = 0; i < 100 && rc == PFKV_OK; i++) {
        snprintf(key, sizeof key, "blob%d", i);
        rc = pfkv_put(&kv, key, v, sizeof v);
    }
    TEST_ASSERT_EQUAL(PFKV_ERR_NOSPC, rc);
    /* Existing data still readable and updatable after hitting the limit. */
    uint8_t out[400];
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_get(&kv, "blob0", out, sizeof out, NULL));
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "blob0", v, 100));
    remount();
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_get(&kv, "blob1", out, sizeof out, NULL));
}

static void test_corrupted_payload_detected(void)
{
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "cal", "abcdef", 6));
    /* Flip a value bit directly in the flash array. */
    for (uint32_t i = 0; i < SS * SC - 6; i++)
        if (memcmp(mem + i, "abcdef", 6) == 0) {
            mem[i] &= 0xFE;
            break;
        }
    char buf[8];
    TEST_ASSERT_EQUAL(PFKV_ERR_CORRUPT, pfkv_get(&kv, "cal", buf, sizeof buf, NULL));
}

static void test_wear_leveling_moves_static_data(void)
{
    pfkv_config_t cfg = { PFKV_GC_GREEDY, 8 };
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_mount(&kv, &hal, &cfg));
    char key[16], v[48];
    memset(v, 'S', sizeof v);
    for (int i = 0; i < 30; i++) {
        snprintf(key, sizeof key, "static%02d", i);
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, key, v, sizeof v));
    }
    for (int i = 0; i < 20000; i++)
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "hot", &i, sizeof i));
    uint32_t mn = UINT32_MAX, mx = 0;
    for (uint32_t s = 0; s < SC; s++) {
        mn = ecs[s] < mn ? ecs[s] : mn;
        mx = ecs[s] > mx ? ecs[s] : mx;
    }
    TEST_ASSERT_TRUE_MESSAGE(mx - mn <= 2 * 8 + 2, "erase spread exceeds threshold");
    remount();
    char out[48];
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_get(&kv, "static07", out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_MEMORY(v, out, sizeof v);
}

static void test_prog_sizes(void)
{
    static const uint32_t sizes[] = { 4, 16, 32 };
    for (int j = 0; j < 3; j++) {
        setup_geometry(512, 4, sizes[j]);
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_mount(&kv, &hal, NULL));
        char v[24];
        for (int i = 0; i < 500; i++) {
            snprintf(v, sizeof v, "v%d", i);
            TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, i % 2 ? "odd" : "even", v, strlen(v)));
        }
        remount();
        assert_value("odd", "v499");
        assert_value("even", "v498");
    }
}

static void test_garbage_sector_is_recovered(void)
{
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "x", "1", 1));
    /* Scribble programmed bits into a free sector, as an aborted erase might. */
    mem[5 * SS + 100] = 0x12;
    remount();
    assert_value("x", "1");
    char v[32];
    for (int i = 0; i < 400; i++) {
        snprintf(v, sizeof v, "%d", i);
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "y", v, strlen(v)));
    }
    remount();
    assert_value("y", "399");
    assert_value("x", "1");
}

static void test_deleted_key_stays_deleted_across_gc(void)
{
    /* A low threshold forces static relocation of sectors that are not the
     * oldest, which must carry the tombstone along. */
    pfkv_config_t cfg = { PFKV_GC_GREEDY, 2 };
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_mount(&kv, &hal, &cfg));
    char key[16], v[40];
    memset(v, 'c', sizeof v);
    /* Old values of "victim" spread over several sectors, then delete it. */
    for (int round = 0; round < 3; round++) {
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "victim", v, sizeof v));
        for (int i = 0; i < 15; i++) {
            snprintf(key, sizeof key, "cold%d_%d", round, i);
            TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, key, v, sizeof v));
        }
    }
    /* Put the tombstone in a mostly-dead sector so greedy GC reclaims it
     * while older values of the key still sit in cold sectors. */
    for (int i = 0; i < 20; i++)
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "hot", &i, sizeof i));
    TEST_ASSERT_EQUAL(PFKV_OK, pfkv_delete(&kv, "victim"));
    for (int i = 0; i < 2000; i++) {
        TEST_ASSERT_EQUAL(PFKV_OK, pfkv_put(&kv, "hot", &i, sizeof i));
        if (i % 97 == 0) {
            TEST_ASSERT_EQUAL(PFKV_OK, pfkv_mount(&kv, &hal, &cfg));
            char buf[64];
            TEST_ASSERT_EQUAL(PFKV_ERR_NOT_FOUND, pfkv_get(&kv, "victim", buf, sizeof buf, NULL));
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sim_program_only_clears_bits);
    RUN_TEST(test_sim_rejects_misaligned_program);
    RUN_TEST(test_sim_cut_kills_device);
    RUN_TEST(test_sim_torn_program_is_partial);
    RUN_TEST(test_put_get_roundtrip);
    RUN_TEST(test_get_missing_key);
    RUN_TEST(test_overwrite_keeps_latest);
    RUN_TEST(test_delete_and_persist);
    RUN_TEST(test_empty_value);
    RUN_TEST(test_buffer_too_small);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_bad_geometry_rejected);
    RUN_TEST(test_gc_reclaims_space_over_many_updates);
    RUN_TEST(test_iterate_visits_live_keys);
    RUN_TEST(test_capacity_limit_returns_nospc);
    RUN_TEST(test_corrupted_payload_detected);
    RUN_TEST(test_wear_leveling_moves_static_data);
    RUN_TEST(test_prog_sizes);
    RUN_TEST(test_garbage_sector_is_recovered);
    RUN_TEST(test_deleted_key_stays_deleted_across_gc);
    return UNITY_END();
}
