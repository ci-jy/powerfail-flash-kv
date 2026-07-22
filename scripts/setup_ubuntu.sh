#!/bin/sh
# Toolchain for host tests, the Cortex-M3 image and the benchmark plots.
set -e
sudo apt-get update
sudo apt-get install -y build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi qemu-system-arm python3 python3-matplotlib
