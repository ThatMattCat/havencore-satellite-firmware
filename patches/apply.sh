#!/usr/bin/env bash
# Re-apply local patches to IDF-managed components after fullclean or
# dependency re-resolution. Idempotent: skips already-applied hunks.
set -euo pipefail
cd "$(dirname "$0")/.."

BSP=managed_components/espressif__esp-box-3/esp-box-3.c
if [ -f "$BSP" ] && ! grep -q "tp_io_config.scl_speed_hz = 0" "$BSP"; then
  patch -p1 -d managed_components/espressif__esp-box-3 \
        < patches/esp-box-3-scl_speed_hz.patch
  echo "applied esp-box-3-scl_speed_hz.patch"
else
  echo "esp-box-3 patch already applied or BSP missing"
fi
