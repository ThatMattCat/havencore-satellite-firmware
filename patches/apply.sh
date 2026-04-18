#!/usr/bin/env bash
# Re-apply local patches to IDF-managed components after fullclean or
# dependency re-resolution. Idempotent: skips already-applied hunks.
set -euo pipefail
cd "$(dirname "$0")/.."

apply_box3_patch() {
  local root="$1"
  local bsp="$root/managed_components/espressif__esp-box-3/esp-box-3.c"
  if [ ! -f "$bsp" ]; then
    echo "$root: esp-box-3 BSP missing, skipping"
    return
  fi
  if grep -q "tp_io_config.scl_speed_hz = 0" "$bsp"; then
    echo "$root: esp-box-3 patch already applied"
    return
  fi
  patch -p1 -d "$root/managed_components/espressif__esp-box-3" \
        < patches/esp-box-3-scl_speed_hz.patch
  echo "$root: applied esp-box-3-scl_speed_hz.patch"
}

apply_box3_patch "."
apply_box3_patch "factory_nvs"
