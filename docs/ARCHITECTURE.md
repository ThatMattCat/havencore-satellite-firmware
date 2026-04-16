# Architecture

This document describes how the firmware is wired today. For the design intent and MVP scope, see [`../plan.md`](../plan.md).

## Hardware

ESP32-S3 (dual-core Xtensa LX7, 240 MHz) on the BOX-3 carrier:

- Dual I2S MEMS mics (front-array, used by ESP-SR's AFE for beamforming + VAD)
- NS4150 speaker amp driven by the ESP32-S3 I2S TX
- 320×240 SPI touch LCD (ILI9342, GT911 touch)
- 16 MB flash, 16 MB PSRAM
- USB-OTG (flashing + serial monitor)

The BSP (`components/bsp/`) is a thin wrapper that selects between the `espressif/esp-box`, `esp-box-3`, and `esp-box-lite` managed components at build time based on `CONFIG_BSP_BOARD_ESP32_S3_BOX_*`.

## Component boundaries

```
main/                    ESP-IDF component "main" — app code
  main.c                 app_main, per-turn orchestration, boot_health_task
  app/
    app_audio.c          I2S RX ring buffer, WAV playback, sr_handler_task
    app_sr.c             ESP-SR AFE + wakenet/VAD pipeline (3 pinned tasks)
    app_wifi.c           Wi-Fi provisioning + event-driven reconnect
    app_ui_ctrl.c        LVGL panel switcher for the four SquareLine screens
    app_ui_events.c      LVGL button callbacks (tap-to-talk, factory reset)
    state.c              turn-level FSM: IDLE/LISTENING/UPLOADING/THINKING/SPEAKING/ERROR
    wake_word.c          runtime gate (default off) for the ESP-SR wakenet branch
    debug_overlay.c      long-press diagnostic overlay (LVGL)
  settings/              NVS wrapper (sys_param_t with ssid/password/url/voice/wake_enabled)
  ui/                    SquareLine-generated LVGL screens (ui_Panel{Sleep,Listen,Get,Reply})

components/bsp/          BOX/BOX-3/BOX-Lite BSP selector wrapper
components/havencore_client/
                         HTTP client for /v1/audio/transcriptions,
                         /v1/chat/completions, /v1/audio/speech,
                         plus havencore_get_ok() for boot health probes
```

## Boot sequence

`app_main()` in `main/main.c`:

1. `nvs_flash_init()` — initialize NVS, erase+reinit if corrupt.
2. `settings_read_parameter_from_nvs()` — read ssid/password/`Base_url`; fall back to UF2 factory partition if missing.
3. `bsp_spiffs_mount()` + `bsp_i2c_init()` + `bsp_display_start_with_config()` — bring up storage, I²C, LCD.
4. `bsp_board_init()` — audio codec, buttons, touch.
5. `ui_ctrl_init()` — LVGL panels + wifi-check timer.
6. `debug_overlay_init()` — install long-press handler on the active screen.
7. `sat_state_init()` — force UI to IDLE (SLEEP panel).
8. `wake_word_set_enabled(sys_param->wake_enabled != 0)` — typically off.
9. `app_network_start()` — kick off Wi-Fi (synchronous until first attempt).
10. `app_sr_start(false)` — spawn ESP-SR feed/detect/handler tasks.
11. `boot_health_task` — one-shot task: waits for Wi-Fi, probes `/api/status`, `/api/stt/health`, `/api/tts/health`; logs results only.

## Turn flow

Trigger: user taps the SLEEP panel. `EventPanelSleepClickCb` → `app_sr_start_once()` sets `manul_detect_flag = true`.

```
audio_detect_task (app_sr.c, pinned core 1)
    sees manul_detect_flag -> posts WAKENET_DETECTED onto result_que
    disables wakenet, starts VAD-watching detect_flag
    on 100 frames of AFE_VAD_SILENCE -> posts ESP_MN_STATE_TIMEOUT

sr_handler_task (app_audio.c, pinned core 0)
    on WAKENET_DETECTED: audio_record_start(), sat_state_set(LISTENING)
    on ESP_MN_STATE_TIMEOUT: audio_record_stop(),
                             if Wi-Fi OK -> start_havencore_turn(record_buffer, len)

start_havencore_turn (main.c)
    sat_state_set(UPLOADING);  havencore_stt   -> transcript
    sat_state_set(THINKING);   havencore_chat  -> reply
    sat_state_set(SPEAKING);   havencore_tts   -> tts_wav
    audio_player_play(fmemopen(tts_wav, ...))
    audio_player idle callback -> sat_state_set(IDLE) via ui_ctrl_reply_set_audio_end_flag

Any leg returning non-OK -> debug_overlay_set_last_error(msg) + sat_state_set(ERROR)
ERROR transitions show the SLEEP panel with a 3000 ms auto-return.
```

## State machine

`main/app/state.h` defines `sat_state_t`. Transitions are centralized in `sat_state_set()`, which maps each state to a SquareLine panel:

| State        | UI panel             | How we enter                             |
| ------------ | -------------------- | ---------------------------------------- |
| IDLE         | `UI_CTRL_PANEL_SLEEP`| boot, end of turn, post-error timeout    |
| LISTENING    | `UI_CTRL_PANEL_LISTEN`| tap-to-talk triggers `sr_handler_task`  |
| UPLOADING    | `UI_CTRL_PANEL_GET`  | VAD silence → start_havencore_turn starts |
| THINKING     | `UI_CTRL_PANEL_GET`  | STT completes                            |
| SPEAKING     | `UI_CTRL_PANEL_REPLY`| chat completes, before TTS request       |
| ERROR        | `UI_CTRL_PANEL_SLEEP` (3 s)| any HTTP failure along the turn    |

Wake-word is gated at the source in `audio_detect_task`: real `WAKENET_DETECTED` events are ignored unless `wake_word_enabled()` returns true, so the ESP-SR pipeline still runs and still provides VAD endpointing, but the wake-word itself never starts a turn. The manual-trigger path (touch → `manul_detect_flag`) re-uses the same code path inside `audio_detect_task` and therefore still works.

## HTTP client contracts

Implemented in `components/havencore_client/havencore_client.c`, using `esp_http_client` + cJSON, 30 s timeout, bodies allocated in PSRAM.

| Function              | Method + path                    | Body                                                      | Response                                |
| --------------------- | -------------------------------- | --------------------------------------------------------- | --------------------------------------- |
| `havencore_stt`       | POST `/v1/audio/transcriptions`  | multipart/form-data: `file=<wav>`, `model=whisper-1`     | JSON with `text` field                  |
| `havencore_chat`      | POST `/v1/chat/completions`      | JSON: single user-role message, no history                | JSON `choices[0].message.content`       |
| `havencore_tts`       | POST `/v1/audio/speech`          | JSON: `input`, `model=tts-1`, `voice`, `response_format=wav` | raw WAV 16 kHz/16-bit mono body      |
| `havencore_get_ok`    | GET `<path>`                     | —                                                         | 2xx = OK; used for boot health probes   |

No TLS, no auth — plan.md assumes a trusted LAN. Agent sessions are server-side (180 s idle timeout), so the device does **not** rebuild chat history on each request.

## UI

SquareLine Studio project lives in `squareline/` and regenerates `main/ui/`. Four panels today — `ui_PanelSleep`, `ui_PanelListen`, `ui_PanelGet`, `ui_PanelReply` — plus setup-wifi and setup-home overlays from the seed.

The debug overlay (`main/app/debug_overlay.c`) is an LVGL object appended to the active screen at init time; a `LV_EVENT_LONG_PRESSED` handler on the screen toggles its `LV_OBJ_FLAG_HIDDEN`. While visible, a 1 Hz timer refreshes live values: current state, base URL, RSSI, free PSRAM, last HTTP error.

## Flash layout

See `partitions.csv`. Notable partitions on 16 MB flash:

| Name         | Offset  | Size    | Use                                    |
| ------------ | ------- | ------- | -------------------------------------- |
| `nvs`        | 0x9000  | 24 KB   | Wi-Fi credentials + agent config       |
| `ota_0`      | 0x700000| 2 MB    | UF2 factory app (provisioning)         |
| `factory`    | 0x10000 | ~6.8 MB | main app image                         |
| `storage`    | 0x900000| 2 MB    | SPIFFS (audio prompts)                 |
| `srmodels`   | 0xB00000| ~5 MB   | ESP-SR wakenet/VAD models              |

Known issue: `ota_0` at 2 MB is smaller than the current app image (~3.5 MB). This is tracked in [ROADMAP.md](ROADMAP.md).
