| Component | .text (flash) | .data | .bss |
|---|---:|---:|---:|
| pfkv core (src/pfkv.c) | 4149 B | 0 B | 0 B |
| FreeRTOS wrapper (pfkv_os.c) | 296 B | 0 B | 0 B |
| Whole demo image (FreeRTOS + tasks + RAM flash) | 15700 B | 4 B | 71808 B |

| RAM / stack | Bytes |
|---|---:|
| `pfkv_t` instance (32 sectors, 128 keys max) | 1528 |
| `pfkv_os_t` instance (store + static mutex) | 1608 |
| Largest library stack frame (`write_record`, -fstack-usage) | 280 |
| Task `writer_counter` peak stack use (FreeRTOS high-water mark) | 656 of 1536 |
| Task `writer_config` peak stack use (FreeRTOS high-water mark) | 656 of 1536 |
| Task `reader` peak stack use (FreeRTOS high-water mark) | 392 of 1536 |
| Task `supervisor` peak stack use (FreeRTOS high-water mark) | 568 of 1536 |
