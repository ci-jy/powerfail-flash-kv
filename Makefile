# Host build: unit tests, power-cut harness, wear benchmark.
# Target build: FreeRTOS image for QEMU mps2-an385 (see firmware/Makefile).

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -Iinclude -Isim -Ithird_party/unity
BUILD   := build/host

LIB_SRC := src/pfkv.c sim/flash_sim.c

.PHONY: all cli test powercut bench plots qemu-test firmware footprint clean

all: $(BUILD)/test_pfkv $(BUILD)/powercut $(BUILD)/wear_bench $(BUILD)/pfkv_cli

$(BUILD):
	mkdir -p $@

$(BUILD)/test_pfkv: tests/test_pfkv.c $(LIB_SRC) third_party/unity/unity.c | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $^

$(BUILD)/powercut: tools/powercut.c $(LIB_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/wear_bench: bench/wear_bench.c bench/naive_store.c $(LIB_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -Ibench -o $@ $^

$(BUILD)/pfkv_cli: examples/pfkv_cli.c $(LIB_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

cli: $(BUILD)/pfkv_cli

test: $(BUILD)/test_pfkv
	./$(BUILD)/test_pfkv

powercut: $(BUILD)/powercut
	./$(BUILD)/powercut --json docs/powercut_results.json

bench: $(BUILD)/wear_bench
	./$(BUILD)/wear_bench > docs/wear_results.json
	python3 scripts/plot_results.py

firmware:
	$(MAKE) -C firmware

qemu-test: firmware
	python3 scripts/qemu_check.py firmware/build/pfkv_demo.elf

footprint: qemu-test
	python3 scripts/footprint.py

clean:
	rm -rf build firmware/build
