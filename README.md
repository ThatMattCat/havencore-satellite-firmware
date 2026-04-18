# HavenCore Satellite Firmware

Firmware for an [ESP32-S3-BOX-3](https://github.com/espressif/esp-box) voice satellite that talks to a self-hosted [HavenCore](https://github.com/ThatMattCat/havencore) agent over a trusted LAN. Tap the screen, speak, hear Selene reply.

Ported from Espressif's `esp-box/examples/chatgpt_demo` with the three OpenAI endpoints (`/v1/audio/transcriptions`, `/v1/chat/completions`, `/v1/audio/speech`) pointed at a HavenCore base URL instead.

## Status

MVP is running on two BOX-3s: tap **or** "Hey Selene" → STT → chat → TTS
→ playback, with status UI and LAN NVS config. Wake-word runs on-device
via a clean-room microWakeWord runtime (replaced ESP-SR wakenet). See
[docs/ROADMAP.md](docs/ROADMAP.md) for known issues and deferred work.

## Repo layout

```
.
├── CMakeLists.txt                  # top-level ESP-IDF project
├── partitions.csv                  # flash layout (16 MB)
├── sdkconfig.defaults              # base Kconfig defaults
├── sdkconfig.ci.box-3              # BOX-3-specific overlay
├── nvs_config.example.csv          # NVS template for provisioning
├── main/                           # app sources
│   ├── main.c                      # app_main + turn orchestration
│   ├── app/                        # audio, SR, UI, wifi, state, wake_word, simple_vad, debug_overlay
│   ├── settings/                   # NVS wrapper (sys_param_t)
│   └── ui/                         # SquareLine-generated LVGL screens
├── components/
│   ├── bsp/                        # esp-box BSP selector wrapper
│   ├── microwakeword/              # on-device "Hey Selene" (streaming int8 TFLite)
│   └── havencore_client/           # HTTP client for /v1/* endpoints
├── model/                          # microWakeWord model + manifest (*.tflite gitignored)
├── factory_nvs/                    # sub-project that builds the TinyUF2 recovery app (→ ota_0)
├── scripts/                        # bootstrap_ota0.sh and related helpers
├── spiffs/                         # prompt/feedback audio assets
├── squareline/                     # SquareLine Studio project (regenerates ui/)
└── docs/
    ├── ARCHITECTURE.md             # how the firmware is wired
    ├── PROVISIONING.md             # UF2 mass-storage (primary) + esptool (appendix)
    ├── SETTINGS.md                 # NVS schema + recipe for new settings
    └── ROADMAP.md                  # planned improvements, deferred work
```

## Build

Requires ESP-IDF v5.5. Activate per shell:

```sh
source ~/esp/v5.5/esp-idf/export.sh
```

Then from the repo root:

```sh
# one-time: build the TinyUF2 recovery sub-project
(cd factory_nvs && idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build)

idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build
```

The top-level build copies `factory_nvs.bin` into the main project's
`build/uf2/` and `idf.py flash` writes it to the `ota_0` partition at
`0x700000`, so a single flash command seeds both the main app (factory)
and the TinyUF2 recovery app. See `docs/PROVISIONING.md`.

## Flash

Device is attached over USB/IP from a remote Windows host; see `~/remote-usb.txt` for the attach procedure. Once `/dev/ttyACM0` appears in this VM:

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

## Provisioning

Primary path: empty NVS (or Settings → factory-reset) →
`settings_factory_reset()` flips boot to the TinyUF2 `ota_0` partition
→ BOX-3 mounts as USB mass-storage → edit `CONFIG.INI`, save, eject.
See [docs/PROVISIONING.md](docs/PROVISIONING.md) for the walkthrough and
[docs/SETTINGS.md](docs/SETTINGS.md) for the full NVS schema.

Required keys:

| Key           | Type    | Notes                                         |
| ------------- | ------- | --------------------------------------------- |
| `ssid`        | string  | target Wi-Fi SSID                             |
| `password`    | string  | Wi-Fi PSK                                     |
| `Base_url`    | string  | plain HTTP only; no `/v1/` suffix             |
| `voice`       | string  | optional, defaults to `af_heart`              |
| `wake_enabled`| string  | optional, `"0"`/`"1"`, defaults to `"1"` (wake word armed); set to `"0"` for touch-to-talk only |

## License

Inherited seed code is `CC0-1.0 OR Unlicense` (Espressif). New code added in this repo is `CC0-1.0` unless noted.
