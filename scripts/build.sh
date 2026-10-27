#!/usr/bin/env bash
# Build (and optionally flash) RTC_Bench with arduino-cli.
#   scripts/build.sh                 build only
#   scripts/build.sh /dev/ttyACM0    build + flash
set -euo pipefail
cd "$(dirname "$0")/../firmware/RTC_Bench"

# USB CDC On Boot must stay Disabled: the console uses the ESP-IDF USB-Serial-JTAG driver
FQBN="esp32:esp32:esp32c5:CDCOnBoot=default,PSRAM=disabled"
BUILD_DIR="${BUILD_DIR:-../../build}"

if [ ! -f credentials.h ]; then
  echo "credentials.h missing: cp credentials_example.h credentials.h and set your WiFi" >&2
  exit 1
fi

arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD_DIR" --warnings default .
if [ $# -ge 1 ]; then
  arduino-cli upload --fqbn "$FQBN" --build-path "$BUILD_DIR" -p "$1" .
fi
