/*
 * pfkv_cli: use the store on a flash image file (8 x 4 KiB sectors, 8-byte
 * program unit), e.g. to prepare or inspect a device settings partition.
 *
 *   pfkv_cli IMAGE put KEY VALUE
 *   pfkv_cli IMAGE get KEY
 *   pfkv_cli IMAGE del KEY
 *   pfkv_cli IMAGE list
 *   pfkv_cli IMAGE stats
 *
 * The image is loaded into the NOR simulator, so every operation follows the
 * same program/erase rules as on the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_sim.h"
#include "pfkv.h"

#define SS 4096u
#define SC 8u
#define PS 8u

static uint8_t mem[SS * SC], units[SS * SC / PS];
static uint32_t ecs[SC];
static flash_sim_t sim;
static pfkv_flash_t hal;
static pfkv_t kv;

static int print_key(const char *key, size_t len, void *arg)
{
    char val[256];
    size_t n = 0;
    (void)len;
    if (pfkv_get((pfkv_t *)arg, key, val, sizeof val - 1, &n) == PFKV_OK) {
        val[n] = 0;
        printf("%s=%s\n", key, val);
    } else {
        printf("%s=<%zu bytes>\n", key, len);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s IMAGE put KEY VALUE | get KEY | del KEY | list | stats\n", argv[0]);
        return 2;
    }
    flash_sim_init(&sim, mem, units, ecs, SS, SC, PS);
    FILE *f = fopen(argv[1], "rb");
    if (f) {
        if (fread(mem, 1, sizeof mem, f) != sizeof mem) {
            fprintf(stderr, "%s: not a %u-byte image\n", argv[1], (unsigned)sizeof mem);
            return 1;
        }
        fclose(f);
        for (uint32_t u = 0; u < sizeof units; u++)
            for (uint32_t i = 0; i < PS; i++)
                units[u] |= mem[u * PS + i] != 0xFF;
    }
    hal = flash_sim_hal(&sim);
    int rc = pfkv_mount(&kv, &hal, NULL);
    const char *cmd = argv[2];
    if (rc == PFKV_OK) {
        if (!strcmp(cmd, "put") && argc == 5)
            rc = pfkv_put(&kv, argv[3], argv[4], strlen(argv[4]));
        else if (!strcmp(cmd, "get") && argc == 4) {
            char val[4096];
            size_t n = 0;
            rc = pfkv_get(&kv, argv[3], val, sizeof val, &n);
            if (rc == PFKV_OK)
                printf("%.*s\n", (int)n, val);
        } else if (!strcmp(cmd, "del") && argc == 4)
            rc = pfkv_delete(&kv, argv[3]);
        else if (!strcmp(cmd, "list"))
            rc = pfkv_iterate(&kv, print_key, &kv);
        else if (!strcmp(cmd, "stats")) {
            printf("keys=%u active_sector=%u write_offset=%u\n", (unsigned)pfkv_count(&kv), (unsigned)kv.active,
                   (unsigned)kv.write_off);
            for (uint32_t s = 0; s < SC; s++)
                printf("sector %u: erase_count=%u live_bytes=%u\n", (unsigned)s, (unsigned)kv.erase_count[s],
                       (unsigned)kv.live_bytes[s]);
        } else {
            fprintf(stderr, "bad command\n");
            return 2;
        }
    }
    if (rc != PFKV_OK) {
        fprintf(stderr, "error %d\n", rc);
        return 1;
    }
    f = fopen(argv[1], "wb");
    if (!f || fwrite(mem, 1, sizeof mem, f) != sizeof mem || fclose(f)) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    return 0;
}
