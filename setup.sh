#!/usr/bin/env bash
# Fetches the third-party libraries into ./lib
set -e
cd "$(dirname "$0")"
mkdir -p lib
[ -d lib/FreeRTOS-Kernel ] || git clone --depth 1 https://github.com/raspberrypi/FreeRTOS-Kernel.git lib/FreeRTOS-Kernel
[ -d lib/minimp3 ]         || git clone --depth 1 https://github.com/lieff/minimp3.git lib/minimp3
[ -d lib/arduino-libhelix ] || git clone --depth 1 https://github.com/pschatzmann/arduino-libhelix.git lib/arduino-libhelix
echo "Libraries ready."
