# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF v5.5 firmware for an ESP32-S3-BOX-3 voice satellite. Ported from Espressif's `esp-box/examples/chatgpt_demo`; the OpenAI endpoints are repointed at a self-hosted HavenCore agent on the LAN. MVP is touch-to-talk: tap → STT → chat → TTS → playback.

`docs/ARCHITECTURE.md` describes how the firmware is currently wired; `docs/ROADMAP.md` tracks MVP verification state, known issues, and deferred work; `docs/PROVISIONING.md` covers the esptool NVS workaround for fresh devices; `docs/SETTINGS.md` is the NVS schema + recipe for adding a new setting (storage, UI row, HTTP plumbing). When changing direction, update ROADMAP first.

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

### Flash layout

`partitions.csv` was re-balanced for the microWakeWord migration: `factory` 6 MB, `ota_0` 5 MB, `storage` (SPIFFS audio prompts) 2 MB @ 0xc00000, `model` (SPIFFS microWakeWord) 1 MB @ 0xe00000. No more `srmodels` partition — ESP-SR's wakenet was ripped out. If you rebalance these again, mind that `ota_0` must stay large enough to hold the TinyUF2 factory app (~3.5 MB) once that flow is fixed.

## Architecture in one pass

The turn orchestration lives in `main/main.c` (`start_havencore_turn`) and drives a small FSM in `main/app/state.c`:

```
tap / "Hey Selene" → audio_detect_task (app_sr.c, core 1)
    → WAKENET_DETECTED onto result_que
    → sr_handler_task (app_audio.c, core 0): record start, LISTENING panel
    → simple_vad silence (≈1.2 s) or 15 s cap → ESP_MN_STATE_TIMEOUT → stop record
    → start_havencore_turn: UPLOADING (STT) → THINKING (chat) → SPEAKING (TTS) → IDLE
    any non-OK leg → debug_overlay_set_last_error + ERROR (SLEEP panel, 3 s auto-return)
```

The audio stack was migrated off ESP-SR: `components/microwakeword/` provides the "Hey Selene" detector (streaming int8 TFLite model loaded from the `model` SPIFFS partition, manifest at `model/hey_selene_v1.json`), and `main/app/simple_vad.c` is a tiny RMS-energy VAD with an adaptive noise floor for listen-window endpointing. `audio_feed_task` pulls stereo I2S, downmixes to mono, and fans the mono stream into both `mww_feed_pcm()` and `simple_vad_feed()` as well as the record buffer. The `audio_detect_task` poll loop gates wake-word triggers on `wake_word_enabled()`; the manual-trigger path (`manul_detect_flag` → tap) always fires.

**Temporary override in `main/app/wake_word.c`:** `wake_word_enabled()` is currently hardcoded to always return `true` and `wake_word_set_enabled()` is a no-op, so NVS `wake_enabled` is effectively ignored. Kept while the UF2 factory-reset flow is broken (see ROADMAP), which means the `esptool` NVS path in `docs/PROVISIONING.md` is the only way to flip the key today. Revert to the NVS-driven gate once UF2 mass-storage provisioning works again.

HTTP client is `components/havencore_client/havencore_client.c` — three endpoints (`/v1/audio/transcriptions`, `/api/chat`, `/v1/audio/speech`) plus `havencore_get_ok()` used by `boot_health_task`. Plain HTTP, no auth. `/api/chat` owns a per-session history window (180 s rolling idle timeout, LRU-pooled) keyed by `X-Session-Id`; on eviction the server returns a new id in the `X-Session-Id` response header, which the client writes back to NVS via the session-changed callback registered in `main.c`. Chat timeout is 60 s to accommodate server-side tool calls.

BSP (`components/bsp/`) is a thin selector between `espressif__esp-box`, `esp-box-3`, and `esp-box-lite` managed components keyed on `CONFIG_BSP_BOARD_ESP32_S3_BOX_*`. SquareLine Studio project lives in `squareline/` and regenerates `main/ui/`; four panels today (`ui_PanelSleep/Listen/Get/Reply`). Long-press on the active screen toggles the debug overlay (`main/app/debug_overlay.c`).

**Hand-edits in SquareLine-generated files.** `main/ui/screens/ui_ScreenSettings.c`, `main/ui/ui.c`, and `main/ui/ui.h` contain manual edits for the Device Name editor (textarea + on-screen keyboard) and the removal of the dead Region Select dropdown. If you regenerate `main/ui/` from `squareline/chat_gpt.spj`, re-apply: (1) the `ui_PanelSettingsDeviceName` / `ui_TextareaSettingsDeviceName` / `ui_KeyboardSettings` block in `ui_ScreenSettings.c`, (2) the `ui_event_TextareaSettingsDeviceName` and `ui_event_KeyboardSettings` handlers in `ui.c`, (3) the back-button keyboard-hide lines, and (4) the matching externs in `ui.h`. Delete any regenerated Region widgets.

Identity headers on every HavenCore request are stamped by `components/havencore_client/`:
- `X-Session-Id: <32-char hex>` — NVS-persisted random blob minted at first boot via `settings_read_parameter_from_nvs()`; loaded into the client through `havencore_client_set_session_id()`. The server may rotate the id on session eviction by echoing a new value in the `X-Session-Id` response header; `havencore_client` captures it, invokes the registered change callback, and `main.c` writes it back to NVS via `settings_set_session_id()`.
- `X-Device-Name: <settings.device_name>` — mirrored from NVS via `havencore_client_set_device_name()` (called at boot and when the user edits the name in Settings).

## Provisioning

NVS keys read by `settings_read_parameter_from_nvs()`: `ssid`, `password`, `Base_url` (required), `voice` (defaults `af_heart`), `wake_enabled` (u8, default 1 — wake word armed if the key is absent; set to 0 to force touch-to-talk only), `device_name` (str, up to 31 chars, default `Satellite` — user-visible room label stamped onto the `X-Device-Name` header; editable in-app via the Settings screen, written back to NVS by `settings_set_device_name()`), `session_id` (str, up to 39 chars — random 32-char hex minted on first boot if absent, rewritten when the server rotates the id in an `/api/chat` response; managed by `settings_set_session_id()`). Missing required keys *should* trigger `settings_factory_reset()` → switch boot to `ota_0` (TinyUF2) → USB mass-storage drive for editing `configuration.nvs`.

The UF2 flow is currently broken (ROADMAP, 2026-04-18). Out-of-the-box devices also arrive with `chatgpt_demo` placeholder values in NVS (`My Network SSID` / `10.0.0.134/v1/`), so the fallback path never fires — the device just loops on Wi-Fi retry. **Provision fresh devices via `esptool` per `docs/PROVISIONING.md`** until UF2 is fixed.
