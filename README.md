# HavenCore Satellite Firmware

ESP-IDF firmware that turns an [**ESP32-S3-BOX-3**](https://github.com/espressif/esp-box)
into a voice client for [**HavenCore**](https://github.com/ThatMattCat/havencore) —
a fully self-hosted, GPU-accelerated home AI. Tap the screen (or say
**"Hey Selene"**), speak, hear the reply. No cloud, no accounts, no per-token bill —
audio goes to a HavenCore agent on your LAN and comes back as TTS.

The wake-word runs on-device, the screen shows what stage the turn is in, and
there are no third-party SaaS dependencies on the edge.

## What it does

```
tap / "Hey Selene"
   → record (microWakeWord trigger, RMS-VAD endpointing, 15 s cap)
   → POST /v1/audio/transcriptions   (Whisper, on the HavenCore box)
   → POST /api/chat                  (Selene agent, full tool calls + memory)
   → POST /v1/audio/speech           (Kokoro TTS)
   → play through the BOX-3 speaker
```

A small FSM drives four LVGL panels (Sleep / Listen / Get / Reply) so you can
glance at it and know what stage you're in. Long-press anywhere to toggle a
debug overlay with the last error, current state, and connectivity.

Per-device identity (`X-Device-Name`, `X-Session-Id`) is sent on every request
so HavenCore can keep room-scoped chat history and route notifications.

## Status

MVP runs daily on two BOX-3s in my house. Wake-word, listen, STT, chat, TTS,
playback, and Wi-Fi/agent provisioning all work end-to-end. Known issues and
deferred work are tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Hardware

- [ESP32-S3-BOX-3](https://www.adafruit.com/product/5805) (16 MB flash, 8 MB PSRAM,
  built-in mic + speaker + 320×240 touchscreen)
- A HavenCore server reachable over plain HTTP on your LAN
- A Wi-Fi network the BOX-3 can join (2.4 GHz)

The BSP is a thin selector wrapper, so the original BOX and BOX-Lite should work
with minor tweaks, but only the **BOX-3** is regularly tested.

## Repo layout

```
.
├── CMakeLists.txt                  # top-level ESP-IDF project
├── partitions.csv                  # 16 MB flash layout
├── sdkconfig.defaults              # base Kconfig defaults
├── sdkconfig.ci.box-3              # BOX-3-specific overlay
├── nvs_config.example.csv          # NVS template for scripted provisioning
├── main/                           # app sources
│   ├── main.c                      #   app_main + turn orchestration
│   ├── app/                        #   audio, SR, UI, wifi, state, wake_word, simple_vad, debug_overlay
│   ├── settings/                   #   NVS wrapper (sys_param_t)
│   └── ui/                         #   SquareLine-generated LVGL screens
├── components/
│   ├── bsp/                        #   esp-box BSP selector wrapper
│   ├── microwakeword/              #   on-device "Hey Selene" (streaming int8 TFLite)
│   └── havencore_client/           #   HTTP client for /v1/* and /api/chat
├── model/                          # microWakeWord model + manifest (*.tflite committed)
├── factory_nvs/                    # sub-project that builds the TinyUF2 recovery app
├── scripts/                        # bootstrap_factory.sh and related helpers
├── spiffs/                         # prompt/feedback audio assets
├── squareline/                     # SquareLine Studio project (regenerates ui/)
├── patches/                        # idempotent patches for managed_components/
└── docs/
    ├── README.md                   #   index of the docs below
    ├── ARCHITECTURE.md             #   hardware, components, boot, FSM, HTTP, UI
    ├── AUDIO.md                    #   wake-word, VAD, listen / follow-up windows
    ├── OTA.md                      #   OTA push/pull, partition layout, gotchas
    ├── PROVISIONING.md             #   UF2 mass-storage flow + esptool appendix
    ├── SETTINGS.md                 #   NVS schema + recipe for new settings
    └── ROADMAP.md                  #   status, known issues, deferred work
```

## Build

Requires **ESP-IDF v5.5**. Activate it once per shell:

```sh
source <path-to-idf>/export.sh
```

After every `idf.py fullclean` or dependency re-resolve, re-apply the BSP patch
(idempotent):

```sh
./patches/apply.sh
```

Then build the main app and the TinyUF2 recovery sub-project:

```sh
# one-time: build the TinyUF2 recovery sub-project
(cd factory_nvs && idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build)

idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build
```

The top-level build copies `factory_nvs.bin` into `build/uf2/`; a single
`idf.py flash` writes both the TinyUF2 recovery app (`factory` @ `0x10000`
— never targeted by OTA because `esp_ota_get_next_update_partition()`
walks `ota_X` only) and the main app (`ota_0` @ `0x190000`, the boot
target after first turn-around). See
[`docs/PROVISIONING.md`](docs/PROVISIONING.md).

## Flash

Plug the BOX-3 in over USB, then:

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

If your BOX-3 is on a different host than your dev machine, USB/IP works fine —
attach the device with `usbipd`/`usbip` until `/dev/ttyACM0` appears locally,
then run the command above.

After the first flash, the dev loop is over the network — no USB:

```sh
make ota IP=10.0.0.42         # push build/havencore_satellite.bin
make version IP=10.0.0.42     # GET /dev/version
```

The device runs an `esp_http_server` on port 80 that exposes `POST /dev/ota`
(refused unless the FSM is IDLE, so a curl mid-conversation just gets HTTP 409)
and `GET /dev/version`. Rollback is enabled — if a fresh image hard-crashes
before `boot_health_task` can mark it valid, the bootloader reverts to the
previous slot automatically.

## Provisioning

Default flow is USB mass-storage — no soldering, no serial console, no
`esptool.py` for end users:

1. Empty NVS (factory) **or** Settings → factory-reset reboots into the
   TinyUF2 partition.
2. The BOX-3 mounts as a USB drive.
3. Edit `CONFIG.INI` (Wi-Fi SSID/PSK, HavenCore base URL, optional voice /
   wake / device name).
4. Save, eject — TinyUF2 writes the values into NVS and boots back into the
   main app.

Required keys:

| Key            | Type   | Notes                                                                                          |
| -------------- | ------ | ---------------------------------------------------------------------------------------------- |
| `ssid`         | string | target Wi-Fi SSID                                                                              |
| `password`     | string | Wi-Fi PSK                                                                                      |
| `Base_url`     | string | HavenCore base URL, plain HTTP, no `/v1/` suffix (e.g. `http://10.0.0.42:6002`)                |
| `voice`        | string | optional, defaults to `af_heart` (any Kokoro voice your server has)                            |
| `wake_enabled` | string | optional, `"0"`/`"1"`, defaults to `"1"`; set `"0"` for touch-to-talk only                     |
| `device_name`  | string | optional, defaults to `Satellite`; sent as `X-Device-Name` so HavenCore can label the room     |

A scripted/recovery `esptool.py` path is documented in the appendix of
[`docs/PROVISIONING.md`](docs/PROVISIONING.md). Full schema and the recipe for
adding a new setting live in [`docs/SETTINGS.md`](docs/SETTINGS.md).

## Companion project

- [**HavenCore**](https://github.com/ThatMattCat/havencore) — the server side:
  agent orchestrator, MCP tool fleet, vLLM/Whisper/Kokoro, dashboard.

## License

Inherited seed code (Espressif's `esp-box/examples/chatgpt_demo`) is
`CC0-1.0 OR Unlicense`. New code added in this repo is `CC0-1.0` unless
otherwise noted in the file. Do whatever you want with it.
