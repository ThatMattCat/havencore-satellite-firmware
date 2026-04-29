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
2. `settings_read_parameter_from_nvs()` — read ssid/password/`Base_url`; on missing required keys, calls `settings_factory_reset()` which flips boot to `factory` (TinyUF2 recovery app — `esp_ota_set_boot_partition(factory)` erases otadata, the bootloader's invalid-otadata fallback then picks factory) and restarts. Also runs one-time migrations (legacy u8 `wake_enabled` → str, erase legacy `ChatGPT_key`).
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
    polls at 20 ms. Three triggers post WAKENET_DETECTED:
      - mww_poll_detected()  [gated by wake_word_enabled()]
      - manul_detect_flag    [tap-to-talk]
      - follow-up window     [silence-first + N-consecutive-speech VAD onset
                              while s_follow_up_deadline_us is in the future,
                              armed by audio_play_finish_cb]
    Locks simple_vad's noise floor for the listen window.
    Ends the window when either silence_ms of silence after the first
      SPEECH frame, or listen_cap_s wall-clock — posts ESP_MN_STATE_TIMEOUT.

sr_handler_task (app_audio.c, pinned core 0)
    on WAKENET_DETECTED: audio_record_start(), sat_state_set(LISTENING)
    on ESP_MN_STATE_TIMEOUT: audio_record_stop(),
                             if Wi-Fi OK -> start_havencore_turn(record_buffer, len)

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

Wake-word is gated at the source in `audio_detect_task`: `mww_poll_detected()` only triggers a LISTENING transition when `wake_word_enabled()` returns true. The feed path keeps calling `mww_feed_pcm()` unconditionally — skipping feeds while listening would tear the streaming model's hidden state. The manual-trigger path (touch → `manul_detect_flag`) is always on and re-uses the same detect-flag flow, so touch-to-talk works even if `wake_word_enabled()` returns false.

The follow-up window adds a third no-wake-word trigger that's open only while `s_follow_up_deadline_us > now`, armed by `audio_play_finish_cb` after a successful TTS playback. To suppress false fires from playback acoustic tail / I2S RX residue, the trigger requires a true silence→speech transition: at least `FOLLOW_UP_SILENCE_FRAMES_REQ` consecutive `SIMPLE_VAD_SILENCE` frames before any `SIMPLE_VAD_SPEECH` is honoured, and then `FOLLOW_UP_SPEECH_FRAMES_REQ` consecutive SPEECH frames before firing. **Do not call `simple_vad_reset()` mid-session** — it slams the noise floor back to `NOISE_INIT` (60), drops the speech threshold to ~240 RMS, and lets ambient room noise instantly trigger SPEECH; preserve the adapted floor across re-arms. See ROADMAP "Follow-up onset tuning" for the post-mortem.

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

The debug overlay (`main/app/debug_overlay.c`) is an LVGL object parented to `lv_layer_top()` (so it floats above whichever SquareLine screen is active); a `LV_EVENT_LONG_PRESSED` handler registered on each panel toggles its `LV_OBJ_FLAG_HIDDEN`. While visible, a 1 Hz timer refreshes live values: current state, running fw version (`esp_app_get_description()->version`, which IDF derives from `git describe --always --tags --dirty`), device IP, base URL, RSSI, free PSRAM, and the last `debug_overlay_set_last_error()` line. A second timer auto-hides the overlay after 15 s; long-press re-arms it.

## Flash layout

See `partitions.csv`. Layout on 16 MB flash (post-OTA rebalance, 2026-04-29):

| Name       | Offset    | Size    | Use                                                                |
| ---------- | --------- | ------- | ------------------------------------------------------------------ |
| `nvs`      | 0x9000    | 16 KB   | Wi-Fi credentials + agent config                                   |
| `otadata`  | 0xd000    | 8 KB    | OTA boot selection (blank → bootloader picks factory)              |
| `phy_init` | 0xf000    | 4 KB    | RF calibration                                                     |
| `factory`  | 0x10000   | 1.5 MB  | TinyUF2 recovery app (factory subtype — never targeted by OTA)     |
| `ota_0`    | 0x190000  | 4 MB    | main app slot A (boot target after first turn-around)              |
| `ota_1`    | 0x590000  | 4 MB    | main app slot B (alternates via `esp_ota_get_next_update_partition`)|
| `storage`  | 0x990000  | 1 MB    | SPIFFS — audio prompts in `spiffs/`                                |
| `model`    | 0xa90000  | 2 MB    | SPIFFS — microWakeWord + future VAD `.tflite` / `.json`            |

`factory` is the only OTA-immune partition subtype reachable via `esp_ota_set_boot_partition()` — `test` would also be OTA-immune but is GPIO-hold-only per the bootloader, and `ota_X` would be overwritten by the normal OTA cycle. So TinyUF2 has to live in `factory`.

Boot flow: on first boot, `otadata` is all 0xff (initial state); the bootloader's invalid-otadata path falls back to `factory` (TinyUF2). TinyUF2's `app_main` immediately calls `esp_ota_set_boot_partition(ota_0)` to schedule the next boot into the main app, then continues into the USB mass-storage UI loop. After the user edits `CONFIG.INI` and reboots, the bootloader follows otadata → ota_0 (main app). OTA writes alternate ota_0 ↔ ota_1; `esp_ota_get_next_update_partition()` walks the OTA chain only, so factory (TinyUF2) is never overwritten.

Software-triggered factory reset: `settings_factory_reset()` calls `esp_ota_set_boot_partition(factory_partition)`, which erases `otadata`. Next boot the bootloader sees invalid otadata and falls back to factory (TinyUF2) again.

`idf.py flash` quirk: parttool.py's `--partition-boot-default` returns the factory offset whenever a factory partition exists, so IDF auto-flashes the project bin to factory by default. We override in `main/CMakeLists.txt` with two `esptool_py_flash_to_partition()` calls — one writes TinyUF2 to factory (last-write-wins on offset 0x10000 in `flasher_args.json`'s deduplicated map), the other writes the main app to `ota_0` explicitly. Side-effect: `idf.py build` prints `Warning: 1/3 app partitions are too small for binary havencore_satellite.bin` for factory — expected, harmless.

Main-app binary is currently 3.46 MB → ~540 KB free in each 4 MB OTA slot. Watch `idf.py size` if AEC + WebSocket land on top of VAD; if free space drops below ~200 KB, repartition (e.g., shrink `model` to 1 MB, grow each app slot to ~4.5 MB). Total flash use today is 12.56 MB; ~3.4 MB unallocated at the top end.

## OTA

`components/havencore_ota/` exposes two paths that both write to whichever slot `esp_ota_get_next_update_partition()` returns (alternating ota_0 ↔ ota_1 — `factory` is OTA-immune):

| Path  | Trigger                              | Endpoint                                   |
| ----- | ------------------------------------ | ------------------------------------------ |
| Push  | `make ota IP=<addr>` (build host)    | `POST http://<device>/dev/ota`            |
| Pull  | Settings → Update Firmware           | `GET ${Base_url}/firmware/satellite.bin`  |

Both check `havencore_ota_state_iface_t::is_idle()` first (the iface is registered in `main.c` and bridges to `sat_state_get() == SAT_STATE_IDLE`); push returns HTTP 409 mid-conversation, pull aborts before allocating buffers.

The push server (`havencore_ota_dev_server_start()`) is started once from `boot_health_task` after the first probe succeeds; it stays up for the lifetime of the app. `GET /dev/version` returns the running app desc as JSON — used by `make version IP=<addr>` and as a debug aid.

The pull path (in `app_ui_events.c::EventButtonSettingsUpdate`) builds two URLs from the configured `Base_url`: `<base>/firmware/satellite.bin` and `<base>/firmware/satellite.json`. The sidecar JSON is fetched first; if its `version` field equals `esp_app_get_description()->version`, the device shows "Up to date" on the GET panel for 2.5 s and reverts to IDLE without entering UPDATING. A missing/unreachable sidecar logs a warning and falls through to an unconditional pull (so servers without sidecar still work). The `havencore_ota_pull()` call wraps `esp_https_ota` and reboots on success.

After a fresh OTA, the freshly-booted image starts in `ESP_OTA_IMG_PENDING_VERIFY`. `boot_health_task` calls `esp_ota_mark_app_valid_cancel_rollback()` after any of the three boot probes (`/api/status`, `/api/stt/health`, `/api/tts/health`) returns 200. A hard-crash before that point causes the bootloader to roll back to the previous slot automatically (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).

### Auto-publish to the web server

`scripts/publish_firmware.sh` runs as a CMake post-build step in `main/CMakeLists.txt` (custom command depending on the .bin output, gated by a stamp file so it only re-runs when the bin changes). The script:

1. Sources `.publish.env` at the repo root (gitignored). If `HC_PUBLISH_DEST` is unset, exits 0 silently.
2. Generates `satellite.json` from `git describe --always --tags --dirty` (matching the version IDF compiles into `esp_app_desc_t`), the bin's byte size, and its sha256.
3. Rsyncs both files to the destination with `--chmod=F644`. Two reasons for rsync over scp: nginx runs as a non-owner uid in its container and would 403 the default mktemp 0600 sidecar, and rsync writes to a tempfile and atomic-renames so a satellite pulling mid-publish gets either the old file or the new one, never a half-written one.

Failures are non-fatal — the build still succeeds; the script prints a warning. A manual `make publish` runs the same script against the current `build/` artifacts.
