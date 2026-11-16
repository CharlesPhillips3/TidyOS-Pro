#!/usr/bin/env bash
# TidyOS Pro - builds everything for the Heltec V4 and V4-R8 and packages it for the web flasher.
#
#   TidyOSPro   TidyOS Pro itself                    -> the "tidyos" app slot
#   TidyBoot    the boot menu (and update installer)  -> the "menu" (factory) slot
#   ShelfDual   The Shelf, built for dual boot        -> the "shelf" app slot (+ its web app in "www")
#
# Needs: arduino-cli, esp32 core 2.0.17 (FabGL doesn't build on 3.x), and the libraries:
#   libraries/FabGL      FabGL 1.0.9 with TidyOS's patches and the ESP32-S3 port (in this repo)
#   RadioLib 7.1.2, U8g2, ESPAsyncWebServer 3.3.23 + ArduinoJson 7 (for The Shelf)
#   source/ShelfDual/libraries (The Shelf's own SD + AsyncTCP)
# The Shelf's web app image comes ready-made from The Shelf's installer site (SHELF_WWW=... to override).
set -euo pipefail
cd "$(dirname "$0")"
VER=$(sed -n 's/^#define TIDYOS_VERSION "\(.*\)"/\1/p' source/TidyOSPro/config.h)
B=${BUILD:-build}; mkdir -p "$B"
CORE="$HOME/Arduino/hardware/espressif/esp32"
TP=()
if [ -d "$CORE/tools/xtensa-esp32s3-elf" ]; then      # core installed by hand rather than Boards Manager
  TP=(--build-property "runtime.tools.esptool_py.path=$CORE/tools/esptool"
      --build-property "runtime.tools.xtensa-esp32s3-elf-gcc.path=$CORE/tools/xtensa-esp32s3-elf"
      --build-property "runtime.tools.xtensa-esp32-elf-gcc.path=$CORE/tools/xtensa-esp32-elf")
  command -v ctags >/dev/null && TP+=(--build-property "runtime.tools.ctags.path=$(dirname "$(command -v ctags)")")
fi
cp source/TidyOSPro/partitions.csv source/TidyBoot/partitions.csv
cp source/TidyOSPro/partitions.csv source/ShelfDual/ShelfV4/partitions.csv

for v in v4 r8; do
  psram=enabled; sfx=""; [ $v = r8 ] && psram=opi && sfx=r8
  F="espressif:esp32:esp32s3:FlashSize=16M,FlashMode=qio,PSRAM=$psram,USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240"
  arduino-cli compile --fqbn "$F" ${TP[@]+"${TP[@]}"} --library "$PWD/libraries/FabGL" \
    --build-property "compiler.cpp.extra_flags=-DTIDYOS_HTML5" \
    --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=ff_disk_read -Wl,--wrap=ff_disk_write -Wl,--wrap=esp_panic_handler" \
    --build-property "upload.maximum_size=3145728" --build-path "$B/b-s3$sfx" source/TidyOSPro
  arduino-cli compile --fqbn "$F" ${TP[@]+"${TP[@]}"} --build-property "upload.maximum_size=458752" --build-path "$B/b-boot$sfx" source/TidyBoot
  arduino-cli compile --fqbn "$F" ${TP[@]+"${TP[@]}"} \
    --build-property "compiler.cpp.extra_flags=-DCONFIG_ASYNC_TCP_RUNNING_CORE=0 -DCONFIG_ASYNC_TCP_STACK_SIZE=16384 -DCONFIG_ASYNC_TCP_QUEUE_SIZE=256 -DSHELF_DUALBOOT" \
    --build-property "upload.maximum_size=2097152" --library "$PWD/source/ShelfDual/libraries/SD" --library "$PWD/source/ShelfDual/libraries/AsyncTCP" \
    --build-path "$B/b-shelf$sfx" source/ShelfDual/ShelfV4
done
rm -f source/TidyBoot/partitions.csv source/ShelfDual/ShelfV4/partitions.csv
# The Shelf's web app (LittleFS image), as published by The Shelf's installer site
S=https://hardwaremack-prog.github.io/The-Shelf-Heltec-V4900mhzMesh/ota
[ -f "$B/shelf-v4-www.bin.gz" ]    || curl -fsSL -o "$B/shelf-v4-www.bin.gz"    "${SHELF_WWW:-$S/shelf-v4-www.bin.gz}"
[ -f "$B/shelf-v4-r8-www.bin.gz" ] || curl -fsSL -o "$B/shelf-v4-r8-www.bin.gz" "${SHELF_WWW_R8:-$S/shelf-v4-r8-www.bin.gz}"
python3 tools/package.py "$VER" "$B" "$B/shelf-v4-www.bin.gz" "$B/shelf-v4-r8-www.bin.gz"
ls -la firmware
