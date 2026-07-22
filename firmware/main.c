/*
 * FreeRTOS stress test for pfkv on QEMU mps2-an385.
 *
 * Two writer tasks and a reader task share one store through the
 * mutex-protected pfkv_os API, over a RAM-backed flash driver with NOR
 * semantics. When the writers finish, a supervisor task remounts the flash
 * from scratch (as after a reset), checks every key, prints stack high-water
 * marks and prints RESULT: PASS or RESULT: FAIL on the UART.
 */
#include <string.h>

#include "FreeRTOS.h"
#include "flash_sim.h"
#include "pfkv.h"
#include "pfkv_os.h"
#include "task.h"

void uart_init(void);
void uart_printf(const char *fmt, ...);
void semihost_exit(int code);

#define SECTOR_SIZE 2048u
#define SECTORS 8u
#define PROG 8u
#define A_WRITES 1500u
#define B_WRITES 400u
#define B_KEYS 6u
#define STACK_WORDS 384u

static uint8_t flash_mem[SECTOR_SIZE * SECTORS];
static uint8_t flash_units[SECTOR_SIZE * SECTORS / PROG];
static uint32_t flash_ecs[SECTORS];
static flash_sim_t flash;
static pfkv_flash_t hal;
static pfkv_os_t store;

static volatile uint32_t errors;
static volatile uint32_t reads_ok;
static volatile int writers_done;
static TaskHandle_t h_sup, h_a, h_b, h_r;

/* Writer B's final view of its keys, checked after the remount. */
typedef struct {
    uint32_t iter, key, pattern, crc;
} cfg_val_t;
static uint32_t b_last[B_KEYS]; /* 0 = deleted / never written */

static void fail(const char *what, uint32_t a, uint32_t b)
{
    errors++;
    uart_printf("error: %s (%u, %u)\n", what, (unsigned)a, (unsigned)b);
}

static void make_cfg(cfg_val_t *v, uint32_t iter, uint32_t key)
{
    v->iter = iter;
    v->key = key;
    v->pattern = iter * 2654435761u ^ key;
    v->crc = pfkv_crc32(0, v, 12);
}

static int cfg_ok(const cfg_val_t *v, uint32_t key)
{
    return v->key == key && v->pattern == (v->iter * 2654435761u ^ key) && v->crc == pfkv_crc32(0, v, 12);
}

static void writer_counter(void *arg)
{
    (void)arg;
    for (uint32_t i = 1; i <= A_WRITES; i++) {
        if (pfkv_os_put(&store, "a/count", &i, sizeof i) != PFKV_OK)
            fail("a put", i, 0);
        uint32_t back = 0;
        if (pfkv_os_get(&store, "a/count", &back, sizeof back, NULL) != PFKV_OK || back != i)
            fail("a readback", i, back);
        if (i % 10 == 0) {
            char key[] = "a/hist0";
            uint32_t hist[4] = { i, i * 3u, i * 7u, ~i };
            key[6] = (char)('0' + (i / 10) % 8);
            if (pfkv_os_put(&store, key, hist, sizeof hist) != PFKV_OK)
                fail("a hist put", i, 0);
        }
        taskYIELD();
    }
    writers_done++;
    xTaskNotifyGive(h_sup);
    vTaskSuspend(NULL);
}

static void writer_config(void *arg)
{
    (void)arg;
    char key[] = "b/k0";
    for (uint32_t i = 1; i <= B_WRITES; i++) {
        uint32_t k = i % B_KEYS;
        cfg_val_t v;
        make_cfg(&v, i, k);
        key[3] = (char)('0' + k);
        if (pfkv_os_put(&store, key, &v, sizeof v) != PFKV_OK)
            fail("b put", i, k);
        else
            b_last[k] = i;
        if (i % 25 == 0) {
            uint32_t d = (i + 3) % B_KEYS;
            key[3] = (char)('0' + d);
            int rc = pfkv_os_delete(&store, key);
            if (rc == PFKV_OK)
                b_last[d] = 0;
            else if (rc != PFKV_ERR_NOT_FOUND || b_last[d] != 0)
                fail("b delete", i, d);
        }
        taskYIELD();
    }
    writers_done++;
    xTaskNotifyGive(h_sup);
    vTaskSuspend(NULL);
}

static int count_key(const char *key, size_t len, void *arg)
{
    (void)key;
    (void)len;
    (*(uint32_t *)arg)++;
    return 0;
}

static void reader(void *arg)
{
    (void)arg;
    uint32_t last = 0;
    char key[] = "b/k0";
    while (writers_done < 2) {
        uint32_t c = 0;
        int rc = pfkv_os_get(&store, "a/count", &c, sizeof c, NULL);
        if (rc == PFKV_OK) {
            if (c < last)
                fail("counter went backwards", last, c);
            last = c;
            reads_ok++;
        } else if (rc != PFKV_ERR_NOT_FOUND) {
            fail("a get", (uint32_t)-rc, 0);
        }
        for (uint32_t k = 0; k < B_KEYS; k++) {
            cfg_val_t v;
            key[3] = (char)('0' + k);
            rc = pfkv_os_get(&store, key, &v, sizeof v, NULL);
            if (rc == PFKV_OK) {
                if (!cfg_ok(&v, k))
                    fail("torn config value", k, v.iter);
                reads_ok++;
            } else if (rc != PFKV_ERR_NOT_FOUND) {
                fail("b get", k, (uint32_t)-rc);
            }
        }
        uint32_t n = 0;
        pfkv_os_iterate(&store, count_key, &n);
        if (n > 1 + 8 + B_KEYS)
            fail("too many keys", n, 0);
        taskYIELD();
    }
    xTaskNotifyGive(h_sup);
    vTaskSuspend(NULL);
}

static void supervisor(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++)
        if (ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(60000)) == 0) {
            fail("timeout", (uint32_t)i, 0);
            break;
        }

    pfkv_stats_t st;
    pfkv_os_stats(&store, &st);
    uart_printf("stress: %u counter writes, %u config writes, %u reads, %u GC runs, %u erases\n",
                A_WRITES, B_WRITES, (unsigned)reads_ok, (unsigned)st.gc_runs, (unsigned)st.erases);

    /* Simulated reset: mount a fresh instance over the same flash. */
    static pfkv_t fresh;
    int rc = pfkv_mount(&fresh, &hal, NULL);
    if (rc != PFKV_OK)
        fail("remount", (uint32_t)-rc, 0);
    uint32_t c = 0;
    if (pfkv_get(&fresh, "a/count", &c, sizeof c, NULL) != PFKV_OK || c != A_WRITES)
        fail("counter after remount", c, A_WRITES);
    char key[] = "b/k0";
    for (uint32_t k = 0; k < B_KEYS; k++) {
        cfg_val_t v;
        key[3] = (char)('0' + k);
        rc = pfkv_get(&fresh, key, &v, sizeof v, NULL);
        if (b_last[k] == 0 ? rc != PFKV_ERR_NOT_FOUND : (rc != PFKV_OK || v.iter != b_last[k] || !cfg_ok(&v, k)))
            fail("config after remount", k, b_last[k]);
    }
    if (flash.violations)
        fail("flash rule violations", flash.violations, 0);
    uart_printf("remount: %u live keys verified\n", (unsigned)pfkv_count(&fresh));

    uart_printf("stack high-water (bytes free of %u): writer_counter=%u writer_config=%u reader=%u supervisor=%u\n",
                (unsigned)(STACK_WORDS * 4u), (unsigned)(uxTaskGetStackHighWaterMark(h_a) * 4u),
                (unsigned)(uxTaskGetStackHighWaterMark(h_b) * 4u), (unsigned)(uxTaskGetStackHighWaterMark(h_r) * 4u),
                (unsigned)(uxTaskGetStackHighWaterMark(NULL) * 4u));
    uart_printf("ram: sizeof(pfkv_t)=%u sizeof(pfkv_os_t)=%u heap_min_free=%u\n", (unsigned)sizeof(pfkv_t),
                (unsigned)sizeof(pfkv_os_t), (unsigned)xPortGetMinimumEverFreeHeapSize());
    uart_printf("RESULT: %s\n", errors ? "FAIL" : "PASS");
    semihost_exit(errors ? 1 : 0);
}

int main(void)
{
    uart_init();
    uart_printf("pfkv FreeRTOS stress test (mps2-an385, Cortex-M3)\n");
    flash_sim_init(&flash, flash_mem, flash_units, flash_ecs, SECTOR_SIZE, SECTORS, PROG);
    hal = flash_sim_hal(&flash);
    int rc = pfkv_os_mount(&store, &hal, NULL);
    if (rc != PFKV_OK) {
        uart_printf("mount failed: %d\nRESULT: FAIL\n", rc);
        semihost_exit(1);
    }
    xTaskCreate(supervisor, "check", STACK_WORDS, NULL, 3, &h_sup);
    xTaskCreate(writer_counter, "wr_cnt", STACK_WORDS, NULL, 2, &h_a);
    xTaskCreate(writer_config, "wr_cfg", STACK_WORDS, NULL, 2, &h_b);
    xTaskCreate(reader, "reader", STACK_WORDS, NULL, 2, &h_r);
    vTaskStartScheduler();
    uart_printf("scheduler exited\nRESULT: FAIL\n");
    semihost_exit(1);
    return 0;
}

void vAssertCalled(const char *file, int line)
{
    uart_printf("assert %s:%d\nRESULT: FAIL\n", file, line);
    semihost_exit(1);
}

void vApplicationStackOverflowHook(TaskHandle_t t, char *name)
{
    (void)t;
    uart_printf("stack overflow in %s\nRESULT: FAIL\n", name);
    semihost_exit(1);
}

void vApplicationMallocFailedHook(void)
{
    uart_printf("malloc failed\nRESULT: FAIL\n");
    semihost_exit(1);
}
