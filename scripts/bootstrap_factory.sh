#!/usr/bin/env bash
#
# One-time UF2 recovery seed for a fresh ESP32-S3-BOX-3.
#
# Flashes factory_nvs.bin into the `factory` partition (offset 0x10000)
# so settings_factory_reset() (and the pre-BSP BOOT-held recovery path)
# can switch boot to the TinyUF2 app, which mounts a USB drive with
# CONFIG.INI for editing NVS.
#
# `factory` is the recovery home because (a) it is never selected by the
# OTA cycle (esp_ota_get_next_update_partition walks ota_X only) so a
# firmware OTA can never overwrite the recovery image, and (b) it is
# reachable via esp_ota_set_boot_partition(factory) which erases otadata,
# triggering the bootloader's "fall back to factory" path.
#
# Also erases the `nvs` partition to wipe the chatgpt_demo placeholder
# values ("My Network SSID", "10.0.0.134/v1/") that ship on fresh devices
# and fool the main app's "required keys" check. This runs once per
# board; `idf.py flash` does not touch NVS on subsequent runs, so
# device_name / session_id / voice / wake_enabled persist.
#
# Usage: scripts/bootstrap_factory.sh [-p /dev/ttyACM0]
#
# Prerequisite: factory_nvs.bin has been built. Run:
#   (cd factory_nvs && idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build)
# or rely on the top-level build, which copies it to
# build/uf2/factory_nvs.bin.

set -euo pipefail

PORT="/dev/ttyACM0"
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port) PORT="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# Prefer the top-level build's copy (produced by esptool_py_flash_to_partition
# wiring in main/CMakeLists.txt). Fall back to factory_nvs's own build dir.
CANDIDATES=(
    "build/uf2/factory_nvs.bin"
    "factory_nvs/build/factory_nvs.bin"
)
FACTORY_NVS_BIN=""
for c in "${CANDIDATES[@]}"; do
    if [[ -f "$c" ]]; then
        FACTORY_NVS_BIN="$c"
        break
    fi
done

if [[ -z "$FACTORY_NVS_BIN" ]]; then
    echo "error: factory_nvs.bin not found. Build it first:" >&2
    echo "  (cd factory_nvs && idf.py -D SDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.ci.box-3\" build)" >&2
    exit 1
fi

echo "port: $PORT"
echo "factory_nvs image: $FACTORY_NVS_BIN"
echo

if ! command -v esptool.py >/dev/null 2>&1; then
    echo "error: esptool.py not on PATH. source ~/esp/v5.5/esp-idf/export.sh" >&2
    exit 1
fi

# Wipe placeholder NVS (one-time). nvs @ 0x9000, size 0x4000.
echo "== erasing nvs (placeholder wipe) =="
esptool.py -p "$PORT" --chip esp32s3 erase_region 0x9000 0x4000

# Seed factory with the TinyUF2 recovery app. factory @ 0x10000.
echo "== flashing factory_nvs.bin -> factory =="
esptool.py -p "$PORT" --chip esp32s3 --flash_size 16MB write_flash \
    0x10000 "$FACTORY_NVS_BIN"

echo
echo "done. Now run: idf.py -p $PORT flash monitor"
echo "The main app will detect missing NVS, switch to UF2, and present a"
echo "USB drive for editing CONFIG.INI."
