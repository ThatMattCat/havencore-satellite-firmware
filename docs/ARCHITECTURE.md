# Architecture

How the firmware is wired today. For deeper subsystem detail, jump to
the topic doc:

- [`AUDIO.md`](AUDIO.md) — wake-word, VAD, follow-up window.
- [`OTA.md`](OTA.md) — OTA push/pull, partition layout, sidecar,
  rollback.
- [`SETTINGS.md`](SETTINGS.md) — NVS schema and the recipe for adding a
  new setting.
- [`PROVISIONING.md`](PROVISIONING.md) — how a fresh device becomes a
  configured satellite.
- [`ROADMAP.md`](ROADMAP.md) — what's left and known issues.

## Hardware

ESP32-S3 (dual-core Xtensa LX7, 240 MHz) on the BOX-3 carrier:

- Dual I2S MEMS mics (front-array, used by ESP-SR's AFE for beamforming + VAD)
- NS4150 speaker amp driven by the ESP32-S3 I2S TX
- 320×240 SPI touch LCD (ILI9342, GT911 touch)
- 16 MB flash, 16 MB PSRAM
- USB-OTG (flashing + serial monitor)

The BSP (`components/bsp/`) is a thin wrapper that selects between the
`espressif/esp-box`, `esp-box-3`, and `esp-box-lite` managed components
at build time based on `CONFIG_BSP_BOARD_ESP32_S3_BOX_*`.

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
components/havencore_ota/
                         OTA push server + pull client; see OTA.md

model/                   microWakeWord artifacts (hey_selene_v1.tflite +
                         manifest); flashed into the "model" SPIFFS partition
```

## Boot sequence

`app_main()` in `main/main.c`:

1. `nvs_flash_init()` — initialize NVS, erase+reinit if corrupt.
2. `settings_read_parameter_from_nvs()` — read ssid/password/`Base_url`; on missing required keys, calls `settings_factory_reset()` which flips boot to `factory` (TinyUF2 recovery app — `esp_ota_set_boot_partition(factory)` erases otadata, the bootloader's invalid-otadata fallback then picks factory) and restarts. Also runs one-time migrations (legacy u8 `wake_enabled` → str, erase legacy `ChatGPT_key`).
3. `bsp_spiffs_mount()` + `bsp_i2c_init()` + `bsp_display_start_with_config()` — bring up storage, I²C, LCD.
4. `bsp_board_init()` — audio codec, buttons, touch.
5. `ui_ctrl_init()` — LVGL panels + wifi-check timer.
6. `debug_overlay_init()` — install long-press handler on the active screen.
7. `sat_state_init()` — force UI to IDLE (SLEEP panel).
8. `wake_word_set_enabled(sys_param->wake_enabled != 0)` — NVS default is `"1"`, so typically on unless explicitly disabled via `CONFIG.INI`.
9. `app_network_start()` — kick off Wi-Fi (synchronous until first attempt).
10. `app_sr_start(false)` — mount the `model` SPIFFS partition, `mww_init()` the wake-word detector, spawn the feed + detect tasks pinned to cores 0/1.
11. `boot_health_task` — one-shot task: waits for Wi-Fi, probes `/api/status`, `/api/stt/health`, `/api/tts/health`; logs results. On the first probe success it (a) calls `esp_ota_mark_app_valid_cancel_rollback()` to commit a freshly-OTA'd image and (b) starts `havencore_ota_dev_server_start()` for `make ota` push updates. See [`OTA.md`](OTA.md).

## Turn flow

Trigger: user taps the SLEEP panel (sets `manul_detect_flag = true`) **or** microWakeWord matches on the live mono feed **or** the post-playback follow-up window detects a fresh speech onset.

```
audio_feed_task    -> downmix stereo I2S to mono, fan out to mww + simple_vad + record buf
audio_detect_task  -> wake/touch/follow-up trigger -> WAKENET_DETECTED
                      end-of-utterance (silence_ms after first speech, or listen_cap_s cap) -> ESP_MN_STATE_TIMEOUT
sr_handler_task    -> WAKENET_DETECTED:    audio_record_start, sat_state_set(LISTENING)
                      ESP_MN_STATE_TIMEOUT: audio_record_stop, start_havencore_turn(buf)

start_havencore_turn (main.c)
    sat_state_set(UPLOADING);  havencore_stt   -> transcript
    sat_state_set(THINKING);   havencore_chat  -> reply
    sat_state_set(SPEAKING);   havencore_tts   -> tts_wav
    audio_player_play(fmemopen(tts_wav, ...))
    audio_player IDLE callback -> audio_play_finish_cb (main.c)
        if follow_up_ms > 0 (and not suppressed by tap-barge):
            sat_state_set(LISTENING) + app_sr_start_follow_up_window(ms)
        else:
            sat_state_set(IDLE)
    Tap during SPEAKING: app_suppress_follow_up_once() + audio_player_stop()
        forces an immediate playback IDLE; the suppression flag prevents
        the follow-up window from auto-arming since the tap is the next
        turn's wake. The existing manul_detect_flag path takes over.

Any leg returning non-OK -> debug_overlay_set_last_error(msg) + sat_state_set(ERROR)
ERROR transitions show a dedicated UI_CTRL_PANEL_ERROR with a 3 s countdown
before auto-returning to IDLE.
```

For the audio-side details (wake-word gating, simple_vad behavior,
follow-up window onset rules and pitfalls), see [`AUDIO.md`](AUDIO.md).

## State machine

`main/app/state.h` defines `sat_state_t`. Transitions are centralized in `sat_state_set()`, which maps each state to a SquareLine panel:

| State        | UI panel             | How we enter                             |
| ------------ | -------------------- | ---------------------------------------- |
| IDLE         | `UI_CTRL_PANEL_SLEEP`| boot, post-playback when `follow_up_ms == 0`, follow-up timeout, post-error timeout |
| LISTENING    | `UI_CTRL_PANEL_LISTEN`| tap, "Hey Selene", or post-playback follow-up window arm |
| UPLOADING    | `UI_CTRL_PANEL_GET`  | VAD silence → start_havencore_turn starts |
| THINKING     | `UI_CTRL_PANEL_GET`  | STT completes                            |
| SPEAKING     | `UI_CTRL_PANEL_REPLY`| chat completes, before TTS request       |
| ERROR        | `UI_CTRL_PANEL_ERROR` (3 s countdown) | any HTTP failure along the turn |

## HTTP client contracts

Implemented in `components/havencore_client/havencore_client.c`, using `esp_http_client` + cJSON, 30 s timeout, bodies allocated in PSRAM.

| Function              | Method + path                    | Body                                                      | Response                                |
| --------------------- | -------------------------------- | --------------------------------------------------------- | --------------------------------------- |
| `havencore_stt`       | POST `/v1/audio/transcriptions`  | multipart/form-data: `file=<wav>`, `model=whisper-1`     | JSON with `text` field                  |
| `havencore_chat`      | POST `/v1/chat/completions`      | JSON: single user-role message, no history                | JSON `choices[0].message.content`       |
| `havencore_tts`       | POST `/v1/audio/speech`          | JSON: `input`, `model=tts-1`, `voice`, `response_format=wav` | raw WAV 16 kHz/16-bit mono body      |
| `havencore_get_ok`    | GET `<path>`                     | —                                                         | 2xx = OK; used for boot health probes   |

No TLS, no auth — the firmware assumes a trusted LAN. Agent sessions
are server-side (180 s idle timeout), so the device does **not**
rebuild chat history on each request.

Every HavenCore request also carries two identity headers stamped by
`set_identity_headers()`: `X-Session-Id` (NVS-persisted, server-rotatable)
and `X-Device-Name` (the user's room label). Full pipeline documented
in [`SETTINGS.md`](SETTINGS.md) § Identity headers.

## UI

SquareLine Studio project lives in `squareline/` and regenerates `main/ui/`. Four panels today — `ui_PanelSleep`, `ui_PanelListen`, `ui_PanelGet`, `ui_PanelReply` — plus setup-wifi and setup-home overlays from the seed.

The debug overlay (`main/app/debug_overlay.c`) is an LVGL object parented to `lv_layer_top()` (so it floats above whichever SquareLine screen is active); a `LV_EVENT_LONG_PRESSED` handler registered on each panel toggles its `LV_OBJ_FLAG_HIDDEN`. While visible, a 1 Hz timer refreshes live values: current state, running fw version (`esp_app_get_description()->version`, which IDF derives from `git describe --always --tags --dirty`), device IP, base URL, RSSI, free PSRAM, and the last `debug_overlay_set_last_error()` line. A second timer auto-hides the overlay after 15 s; long-press re-arms it.

The Settings screen (gear icon → Settings) is a fifth, hand-edited
panel that exposes user-editable NVS keys (Device Name, Listen Cap,
Silence Timeout, Follow-Up Window, Update Firmware). See
[`SETTINGS.md`](SETTINGS.md) for the row pattern and the SquareLine
regeneration hazard.
