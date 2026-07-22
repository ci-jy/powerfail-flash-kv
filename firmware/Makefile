# FreeRTOS image for QEMU mps2-an385 (Cortex-M3).
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
SIZE    := $(CROSS)size
ROOT    := ..
RTOS    := $(ROOT)/third_party/freertos
BUILD   := build
ARCH    := -mcpu=cortex-m3 -mthumb
CFLAGS  := $(ARCH) -std=c11 -Os -g -ffunction-sections -fdata-sections -Wall -Wextra \
           -I. -I$(ROOT)/include -I$(ROOT)/sim -I$(ROOT)/port/freertos \
           -I$(RTOS)/include -I$(RTOS)/portable/GCC/ARM_CM3
LDFLAGS := $(ARCH) -T mps2_an385.ld -nostartfiles --specs=nano.specs --specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/pfkv_demo.map

LIB_SRC  := $(ROOT)/src/pfkv.c $(ROOT)/port/freertos/pfkv_os.c
APP_SRC  := main.c startup.c uart.c $(ROOT)/sim/flash_sim.c
RTOS_SRC := $(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c \
            $(RTOS)/portable/GCC/ARM_CM3/port.c $(RTOS)/portable/MemMang/heap_4.c
SRC := $(LIB_SRC) $(APP_SRC) $(RTOS_SRC)
OBJ := $(patsubst %.c,$(BUILD)/obj/%.o,$(subst $(ROOT)/,,$(SRC)))

all: $(BUILD)/pfkv_demo.elf

$(BUILD)/obj/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fstack-usage -c -o $@ $<

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fstack-usage -c -o $@ $<

$(BUILD)/pfkv_demo.elf: $(OBJ) mps2_an385.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJ)
	$(SIZE) $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
