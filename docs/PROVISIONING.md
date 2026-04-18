# Provisioning

How to get a fresh ESP32-S3-BOX-3 from flashed-but-empty NVS to a working
HavenCore satellite. The primary path is TinyUF2 mass-storage in `ota_0`
— device reboots into a USB drive, you edit `CONFIG.INI`, and normal
boot resumes with real values. The esptool/CSV path from earlier is
still here as an appendix for dev work and recovery.

## Primary path: TinyUF2 mass-storage

### 1. One-time first flash per board

```sh
source ~/esp/v5.5/esp-idf/export.sh
./patches/apply.sh                      # re-apply BSP touch patch if needed
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build
idf.py -p /dev/ttyACM0 flash monitor
```

The top-level build writes both apps: the main firmware to `factory`
(0x10000) and the TinyUF2 recovery app to `ota_0` (0x700000), via the
`esptool_py_flash_to_partition(flash "ota_0" ...)` wiring in
`main/CMakeLists.txt`. No separate esptool step.

Pristine device with empty NVS: `settings_read_parameter_from_nvs()`
fails the "required keys" check, calls `settings_factory_reset()`, and
the device auto-reboots into UF2. Jump to step 3.

Device with old `chatgpt_demo` placeholder NVS (`My Network SSID` /
`10.0.0.134/v1/`): Wi-Fi will fail but the UI still comes up on the
Sleep panel. Continue to step 2.

### 2. Trigger factory-reset from the UI

From the Sleep panel:
1. Tap the gear / Settings icon.
2. Tap the factory-reset icon and confirm.

`settings_factory_reset()` switches the boot partition to `ota_0` and
restarts into the TinyUF2 app.

### 3. Edit CONFIG.INI on the mounted drive

The host sees a new USB mass-storage device (typical label: `BOX3BOOT`
or similar). Open `CONFIG.INI` and set the keys under `[configuration]`:

```ini
[configuration]
ssid=Your-Wifi-SSID
password=your-wifi-psk
Base_url=http://selene.renman.wtf
voice=af_heart
wake_enabled=1
```

Note on `Base_url`:
- Plain HTTP only. The firmware has no TLS.
- Do **not** include the `/v1/` suffix. `build_url()` strips it, but
  also logs a one-shot warning — just leave it off.

Keys NOT supplied in CONFIG.INI keep their existing NVS values (if any)
or fall back to defaults in `settings.c`:
- `voice` → `af_heart`
- `wake_enabled` → `1`
- `device_name` → `Satellite` (editable in-app via Settings)
- `session_id` → minted on first main-app boot

Save and eject the drive cleanly. TinyUF2 writes the new keys into NVS,
switches the boot partition back to `factory`, and resets.

### 4. Verify

```
settings: stored ssid:<your ssid>
settings: stored Base URL:http://selene.renman.wtf
...
wifi:<ba-add>...got ip:10.0.0.XXX
boot_health: /api/status -> HTTP 200
boot_health: /api/stt/health -> HTTP 200
boot_health: /api/tts/health -> HTTP 200
```

### Re-provisioning later

Any time you need to change Wi-Fi creds, `Base_url`, voice, or
`wake_enabled`: Sleep → Settings → factory-reset → edit CONFIG.INI →
eject. Same flow. NVS isn't wiped, only the keys you rewrite change.

## Recovery: `scripts/bootstrap_ota0.sh`

Thin wrapper around the esptool calls for devices whose `ota_0` is
blank/corrupt. Normally you don't need this — `idf.py flash` seeds
`ota_0` automatically. Keep it around for:

- A board that was flashed with an older firmware revision that didn't
  include the `esptool_py_flash_to_partition` wiring.
- `ota_0` got erased somehow (manual `erase_region`, partition-table
  reshuffle).

```sh
source ~/esp/v5.5/esp-idf/export.sh
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build
scripts/bootstrap_ota0.sh -p /dev/ttyACM0
```

The script also erases the `nvs` partition (0x9000, 0x4000) to clear
chatgpt_demo placeholder values. That's one-time, destructive data loss
for any existing `device_name` / `session_id` on that board. Plain
`idf.py flash` does not erase NVS.

## Appendix: direct NVS write with esptool

For cases where the UF2 flow isn't an option — scripted mass provisioning,
CI fixtures, or recovering a device that's too wedged to enter UF2.

### 1. Make a config CSV

```sh
cp nvs_config.example.csv nvs_config.csv
$EDITOR nvs_config.csv
```

`nvs_config.csv` is gitignored — it contains your Wi-Fi PSK. Expected shape:

```csv
key,type,encoding,value
configuration,namespace,,
ssid,data,string,Your-Wifi-SSID
password,data,string,your-wifi-psk
Base_url,data,string,http://selene.renman.wtf
voice,data,string,af_heart
wake_enabled,data,string,1
```

### 2. Generate and flash the NVS binary

```sh
source ~/esp/v5.5/esp-idf/export.sh

python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
    generate nvs_config.csv nvs_config.bin 0x4000

python -m esptool -p /dev/ttyACM0 --chip esp32s3 \
    write_flash 0x9000 nvs_config.bin
```

### 3. Reset and verify

```sh
idf.py -p /dev/ttyACM0 monitor
```

Settings log should reflect your values, followed by a DHCP lease and
three 200-OK health probes.
