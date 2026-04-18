## factory_nvs: TinyUF2 recovery app for the HavenCore satellite

This is the "factory app" that lives in the `ota_0` partition. When the
main app detects missing or empty required NVS keys (or the user forces
it via the Settings-screen reset icon / holding BOOT during boot), it
flips the boot partition to `ota_0` and restarts. This app then runs,
exposing the `nvs/configuration` namespace over USB mass-storage as a
`CONFIG.INI` file (via [esp_tinyuf2](https://github.com/espressif/esp-iot-solution/tree/master/components/usb/esp_tinyuf2)).
Edit the file, save, unplug — the app pre-armed the next boot to return
to the main `factory` app, which picks up the new values.

Normally seeded automatically: the main project's `main/CMakeLists.txt`
uses `esptool_py_flash_to_partition(flash "ota_0" ...)` so `idf.py flash`
writes both the main app *and* this recovery app in one pass. The
standalone flash via `scripts/bootstrap_ota0.sh` below is only needed
for devices whose `ota_0` is blank/corrupt (e.g. first-time setup of a
board that was flashed with a firmware revision predating that wiring).

## Build

Source the IDF env (`source ~/esp/v5.5/esp-idf/export.sh`) then:

```sh
cd factory_nvs
idf.py fullclean
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build
```

The BOX-3 override in `sdkconfig.ci.box-3` selects the correct BSP; the
shared `components/bsp` selector lives at `../components/bsp` and keys
on `CONFIG_BSP_BOARD_ESP32_S3_BOX_3`.

## Flash (recovery only)

Use the top-level helper for manual seeding:

```sh
./scripts/bootstrap_ota0.sh -p /dev/ttyACM0
```

The script wipes the `nvs` partition (clearing any chatgpt_demo
placeholder values) and writes `build/factory_nvs.bin` to `0x700000`.
Under the hood:

```sh
python -m esptool -p /dev/ttyACM0 --chip esp32s3 erase_region 0x9000 0x4000
python -m esptool -p /dev/ttyACM0 --chip esp32s3 -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 16MB --flash_freq 80m \
    0x700000 build/factory_nvs.bin
```

After that, flash the main app normally (`idf.py -p /dev/ttyACM0 flash
monitor` in the repo root). With empty NVS, the main app detects
missing required keys and auto-reboots into the UF2 recovery flow so
you can set `ssid` / `password` / `Base_url` via USB mass-storage.
