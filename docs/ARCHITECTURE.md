# Architecture

This document describes how the firmware is wired today. For what's left
and deferred work, see [`ROADMAP.md`](ROADMAP.md).

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
    app_sr.c             audio feed/detect tasks: I2S → downmix →
                         microwakeword + simple_vad + record buffer
    simple_vad.c         tiny RMS-energy VAD with adaptive noise floor
                         (listen-window silence cutoff; replaces AFE VAD)
    app_wifi.c           Wi-Fi provisioning + event-driven reconnect
    app_ui_ctrl.c        LVGL panel switcher for the four SquareLine screens
    app_ui_events.c      LVGL button callbacks (tap-to-talk, factory reset)
    state.c              turn-level FSM: IDLE/LISTENING/UPLOADING/THINKING/SPEAKING/ERROR
    wake_word.c          runtime gate for the microWakeWord detector
                         (NVS-backed via settings.wake_enabled)
    debug_overlay.c      long-press diagnostic overlay (LVGL)
  settings/              NVS wrapper (sys_param_t with ssid/password/url/voice/wake_enabled)
  ui/                    SquareLine-generated LVGL screens (ui_Panel{Sleep,Listen,Get,Reply})

components/bsp/          BOX/BOX-3/BOX-Lite BSP selector wrapper
components/microwakeword/
                         clean-room microWakeWord runtime:
                         int8 TFLite streaming model + micro_speech frontend,
                         16 kHz mono PCM in → mww_poll_detected()
components/havencore_client/
                         HTTP client for /v1/audio/transcriptions,
                         /v1/chat/completions, /v1/audio/speech,
                         plus havencore_get_ok() for boot health probes

model/                   microWakeWord artifacts (hey_selene_v1.tflite +
                         manifest); flashed into the "model" SPIFFS partition
```

## Boot sequence

`app_main()` in `main/main.c`:

1. `nvs_flash_init()` — initialize NVS, erase+reinit if corrupt.
2. `settings_read_parameter_from_nvs()` — read ssid/password/`Base_url`; on missing required keys, calls `settings_factory_reset()` which flips boot to `ota_0` (TinyUF2 recovery app) and restarts. Also runs one-time migrations (legacy u8 `wake_enabled` → str, erase legacy `ChatGPT_key`).
3. `bsp_spiffs_mount()` + `bsp_i2c_init()` + `bsp_display_start_with_config()` — bring up storage, I²C, LCD.
4. `bsp_board_init()` — audio codec, buttons, touch.
5. `ui_ctrl_init()` — LVGL panels + wifi-check timer.
6. `debug_overlay_init()` — install long-press handler on the active screen.
7. `sat_state_init()` — force UI to IDLE (SLEEP panel).
8. `wake_word_set_enabled(sys_param->wake_enabled != 0)` — NVS default is `"1"`, so typically on unless explicitly disabled via `CONFIG.INI`.
9. `app_network_start()` — kick off Wi-Fi (synchronous until first attempt).
10. `app_sr_start(false)` — mount the `model` SPIFFS partition, `mww_init()` the wake-word detector, spawn the feed + detect tasks pinned to cores 0/1.
11. `boot_health_task` — one-shot task: waits for Wi-Fi, probes `/api/status`, `/api/stt/health`, `/api/tts/health`; logs results only.

## Turn flow

Trigger: user taps the SLEEP panel (sets `manul_detect_flag = true`) **or** microWakeWord matches on the live mono feed.

```
audio_feed_task (app_sr.c, pinned core 0)
    reads stereo I2S (20 ms / 320 samples / 1280 B)
    downmixes to mono (right slot — both channels carry the same room mic)
    fans out: mww_feed_pcm() + simple_vad_feed() + audio_record_save()

audio_detect_task (app_sr.c, pinned core 1)
    polls at 20 ms. Either mww_poll_detected() [gated by wake_word_enabled()]
      or manul_detect_flag flips detect_flag=true and posts WAKENET_DETECTED.
    Locks simple_vad's noise floor for the listen window.
    Ends the window when either 60 silence frames (≈1.2 s) after the first
      SPEECH frame, or 15 s wall-clock — posts ESP_MN_STATE_TIMEOUT.

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
ERROR transitions show a dedicated UI_CTRL_PANEL_ERROR with a 3 s countdown
before auto-returning to IDLE.
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
| ERROR        | `UI_CTRL_PANEL_ERROR` (3 s countdown) | any HTTP failure along the turn |

Wake-word is gated at the source in `audio_detect_task`: `mww_poll_detected()` only triggers a LISTENING transition when `wake_word_enabled()` returns true. The feed path keeps calling `mww_feed_pcm()` unconditionally — skipping feeds while listening would tear the streaming model's hidden state. The manual-trigger path (touch → `manul_detect_flag`) is always on and re-uses the same detect-flag flow, so touch-to-talk works even if `wake_word_enabled()` returns false.

## HTTP client contracts

Implemented in `components/havencore_client/havencore_client.c`, using `esp_http_client` + cJSON, 30 s timeout, bodies allocated in PSRAM.

| Function              | Method + path                    | Body                                                      | Response                                |
| --------------------- | -------------------------------- | --------------------------------------------------------- | --------------------------------------- |
| `havencore_stt`       | POST `/v1/audio/transcriptions`  | multipart/form-data: `file=<wav>`, `model=whisper-1`     | JSON with `text` field                  |
| `havencore_chat`      | POST `/v1/chat/completions`      | JSON: single user-role message, no history                | JSON `choices[0].message.content`       |
| `havencore_tts`       | POST `/v1/audio/speech`          | JSON: `input`, `model=tts-1`, `voice`, `response_format=wav` | raw WAV 16 kHz/16-bit mono body      |
| `havencore_get_ok`    | GET `<path>`                     | —                                                         | 2xx = OK; used for boot health probes   |

No TLS, no auth — the firmware assumes a trusted LAN. Agent sessions are server-side (180 s idle timeout), so the device does **not** rebuild chat history on each request.

## UI

SquareLine Studio project lives in `squareline/` and regenerates `main/ui/`. Four panels today — `ui_PanelSleep`, `ui_PanelListen`, `ui_PanelGet`, `ui_PanelReply` — plus setup-wifi and setup-home overlays from the seed.

The debug overlay (`main/app/debug_overlay.c`) is an LVGL object appended to the active screen at init time; a `LV_EVENT_LONG_PRESSED` handler on the screen toggles its `LV_OBJ_FLAG_HIDDEN`. While visible, a 1 Hz timer refreshes live values: current state, base URL, RSSI, free PSRAM, last HTTP error.

## Flash layout

See `partitions.csv`. Layout on 16 MB flash (post-microWakeWord rebalance):

| Name      | Offset    | Size    | Use                                                    |
| --------- | --------- | ------- | ------------------------------------------------------ |
| `nvs`     | 0x9000    | 16 KB   | Wi-Fi credentials + agent config                       |
| `otadata` | 0xd000    | 8 KB    | OTA boot selection                                     |
| `phy_init`| 0xf000    | 4 KB    | RF calibration                                         |
| `factory` | 0x10000   | 6 MB    | main app image                                         |
| `ota_0`   | 0x700000  | 5 MB    | TinyUF2 factory app (provisioning fallback)            |
| `storage` | 0xc00000  | 2 MB    | SPIFFS — audio prompts in `spiffs/`                    |
| `model`   | 0xe00000  | 1 MB    | SPIFFS — microWakeWord `.tflite` + `.json` in `model/` |

No more `srmodels` partition — ESP-SR was removed in the microWakeWord migration. `ota_0` is kept large enough for the TinyUF2 recovery app (currently ~1 MB; 5 MB leaves headroom for future growth). Shrinking it below the recovery image size would break UF2 provisioning.
