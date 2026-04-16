# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF v5.5 firmware for an ESP32-S3-BOX-3 voice satellite. Ported from Espressif's `esp-box/examples/chatgpt_demo`; the OpenAI endpoints are repointed at a self-hosted HavenCore agent on the LAN. MVP is touch-to-talk: tap → STT → chat → TTS → playback.

`plan.md` at the repo root is the authoritative spec. `docs/ARCHITECTURE.md` describes how the firmware is currently wired; `docs/ROADMAP.md` tracks known issues and deferred work. Update `plan.md` first when changing direction.

## Build & flash

ESP-IDF v5.5 is at `~/esp/v5.5/esp-idf`. `$IDF_PATH` is not preset — `source ~/esp/v5.5/esp-idf/export.sh` in each new shell.

```sh
# one-time (builds factory_nvs.bin used for first-boot provisioning)
cd factory_nvs && idf.py build && cd ..

idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build
idf.py -p /dev/ttyACM0 flash monitor
```

The BOX-3 device is attached from a remote Windows host (10.0.0.88) via usbipd/usbip — see `~/remote-usb.txt` for the attach sequence before `/dev/ttyACM0` appears.

### The `managed_components` patch

`managed_components/` is gitignored and regenerated on `idf.py fullclean` or dependency re-resolve. After either, run `./patches/apply.sh` — it re-applies `patches/esp-box-3-scl_speed_hz.patch`, which clears `tp_io_config.scl_speed_hz` so `bsp_touch_new()` doesn't crash on the legacy `esp_lcd_new_panel_io_i2c_v1` path. The script is idempotent.

### Flash layout caveat

`partitions.csv` gives `ota_0` 2 MB but the current app image is ~3.5 MB. This currently blocks flashing; see `docs/ROADMAP.md`. Don't just enlarge `ota_0` casually — the layout is packed around a 16 MB flash and the `srmodels` / SPIFFS partitions matter.

## Architecture in one pass

The turn orchestration lives in `main/main.c` (`start_havencore_turn`) and drives a small FSM in `main/app/state.c`:

```
tap → manul_detect_flag (app_sr.c audio_detect_task, core 1)
    → WAKENET_DETECTED onto result_que
    → sr_handler_task (app_audio.c, core 0): record start, LISTENING panel
    → VAD silence (≈1.2 s) → ESP_MN_STATE_TIMEOUT → stop record
    → start_havencore_turn: UPLOADING (STT) → THINKING (chat) → SPEAKING (TTS) → IDLE
    any non-OK leg → debug_overlay_set_last_error + ERROR (SLEEP panel, 3 s auto-return)
```

Wake-word is currently a runtime gate stub (`main/app/wake_word.c`) defaulting off — real `WAKENET_DETECTED` events are filtered in `audio_detect_task` unless `wake_word_enabled()` returns true. The ESP-SR AFE still runs and supplies VAD endpointing for the manual path.

HTTP client is `components/havencore_client/havencore_client.c` — three endpoints (`/v1/audio/transcriptions`, `/v1/chat/completions`, `/v1/audio/speech`) plus `havencore_get_ok()` used by `boot_health_task`. Plain HTTP, no auth, no history rebuild — the HavenCore server keeps session state for 180 s.

BSP (`components/bsp/`) is a thin selector between `espressif__esp-box`, `esp-box-3`, and `esp-box-lite` managed components keyed on `CONFIG_BSP_BOARD_ESP32_S3_BOX_*`. SquareLine Studio project lives in `squareline/` and regenerates `main/ui/`; four panels today (`ui_PanelSleep/Listen/Get/Reply`). Long-press on the active screen toggles the debug overlay (`main/app/debug_overlay.c`).

## Provisioning

NVS keys read by `settings_read_parameter_from_nvs()`: `ssid`, `password`, `Base_url` (required), `voice` (defaults `af_heart`), `wake_enabled` (u8, default 0). Missing required keys → boot switches to the UF2 factory partition, which exposes mass-storage for editing `configuration.nvs`.
