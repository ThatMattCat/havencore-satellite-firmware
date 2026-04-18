# Provisioning

How to get a fresh ESP32-S3-BOX-3 from flashed-but-empty NVS to a working
HavenCore satellite. The intended path was the TinyUF2 mass-storage fallback
in `ota_0`, but that flow is currently broken (see
[ROADMAP.md](ROADMAP.md) → "UF2 factory-reset flow"). Until it's fixed,
provision directly with `esptool`.

## Symptoms that tell you NVS is wrong

On boot, `main/settings/settings.c` logs the values it read:

```
settings: stored ssid:My Network SSID
settings: stored password:My Password
settings: stored Base URL:http://10.0.0.134/v1/
settings: voice:af_heart wake_enabled:1
```

If you see the `My Network SSID` / `My Password` / `10.0.0.134/v1/`
placeholders, NVS was seeded from the upstream `chatgpt_demo` factory app
and the device will loop on `sta disconnect, retry attempt N`. These keys
are not empty, so `settings_factory_reset()` does **not** fire — it doesn't
fall through to the UF2 partition. You have to overwrite NVS yourself.

## The workaround: write NVS directly with esptool

### 1. Make a config CSV

Copy the example and fill in real values:

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
wake_enabled,data,u8,1
```

Note on `Base_url`:
- Plain HTTP only. The firmware has no TLS (`mbedtls` bundle isn't enabled).
- Do **not** include the `/v1/` suffix. `build_url()` strips it, but also
  logs a one-shot warning when it does — just leave it off.

### 2. Generate and flash the NVS binary

```sh
source ~/esp/v5.5/esp-idf/export.sh

# 0x4000 matches the nvs partition size in partitions.csv
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
    generate nvs_config.csv nvs_config.bin 0x4000

# flash at the nvs partition offset
python -m esptool -p /dev/ttyACM0 --chip esp32s3 \
    write_flash 0x9000 nvs_config.bin
```

### 3. Reset and verify

```sh
idf.py -p /dev/ttyACM0 monitor
```

You should see the settings log reflect your real values, followed by a
successful DHCP lease and the three health probes returning 200:

```
settings: stored ssid:Renman
settings: stored Base URL:http://selene.renman.wtf
...
wifi:<ba-add>idx:0 (ifx:0, ...) got ip:10.0.0.147
boot_health: /api/status -> HTTP 200
boot_health: /api/stt/health -> HTTP 200
boot_health: /api/tts/health -> HTTP 200
```

## When to re-provision

- New out-of-the-box device (seed `chatgpt_demo` placeholders are present).
- Server moved (e.g. old `ai.renman.wtf` → new `selene.renman.wtf`).
- Wi-Fi credentials rotated.
- Toggling `wake_enabled` on/off. Currently moot because `wake_word.c`
  hardcodes the flag to `true` as a stopgap — see the 2026-04-18 note in
  ROADMAP — but once that revert lands, flipping the NVS key is the only
  way.

## Once the UF2 flow is fixed

The intended provisioning UX is:

1. Device with missing/empty NVS keys boots → `settings_factory_reset()` flips
   the boot partition to `ota_0` (TinyUF2) and restarts.
2. BOX-3 appears on the host as a USB mass-storage drive.
3. User edits `configuration.nvs` on that drive and resets.

When that's working again, prefer it for end-user setup. The
`esptool`/CSV path above is only the dev workaround.
