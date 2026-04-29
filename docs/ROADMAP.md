# Roadmap

Tracks known issues, deferred work, and planned improvements. MVP scope
(touch + "Hey Selene" → STT → chat → TTS → playback, with status UI and
NVS provisioning) has landed; this doc captures what's left on top.

## Current status (2026-04-29)

Both BOX-3 devices running the `updates` branch against
`http://selene.renman.wtf`. Full turn round-trip (tap → STT → chat → TTS)
has been exercised. UF2 factory-reset flow repaired 2026-04-18: Settings →
factory-reset switches boot to the TinyUF2 recovery slot (now
`uf2_recov`, see partition note below) and TinyUF2 mounts a USB drive
for editing `CONFIG.INI`. Listen-window tunables (`listen_cap_s`,
`silence_ms`) shipped 2026-04-21 as user-editable Settings sliders.

OTA infrastructure landed 2026-04-29 with both paths live (push from
the build host for the dev loop, pull from the Settings screen for
end-user updates). Partition table re-balanced for A/B OTA: `factory`
1.5 MB (TinyUF2 — only OTA-immune subtype reachable via
`esp_ota_set_boot_partition`; `test` is GPIO-hold-only, `ota_X` would be
overwritten), `ota_0` + `ota_1` 4 MB each (main app A/B), 1 MB
`storage`, 2 MB `model`. Dev loop is `make ota IP=<addr>` from the
build host — `esp_http_server` on port 80 of the device exposes
`POST /dev/ota` (refused unless state is IDLE) and `GET /dev/version`.
Rollback enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`);
`boot_health_task` calls `esp_ota_mark_app_valid_cancel_rollback()`
after the first boot probe returns 200. End-user pull path: Settings →
Update Firmware fetches `${Base_url}/firmware/satellite.json` (a
`{version, size, sha256}` sidecar published alongside the bin), and if
the sidecar's `version` matches `esp_app_get_description()->version`
the device shows "Up to date" and skips the pull entirely. A missing or
unreachable sidecar falls through to an unconditional pull. Build host
auto-publishes both bin and sidecar via a CMake post-build hook calling
`scripts/publish_firmware.sh` — rsyncs over ssh with `--chmod=F644`
(nginx runs as a non-owner uid; the default mktemp 0600 sidecar would
otherwise 403) and atomic-renames so a satellite pulling mid-publish
sees either the old or new file, never a half-written one. Configure
via `.publish.env` at the repo root; missing → silent no-op.

### OTA gotchas worth knowing

- **`app, test` subtype isn't software-bootable.** First pass put TinyUF2
  in a `test` partition and called `esp_ota_set_boot_partition()` on
  it, expecting otadata to point there. `esp_ota_set_boot_partition()`
  for non-factory subtypes calls `esp_rewrite_ota_data(subtype)`, and
  `SUB_TYPE_ID(0x20)` is `0` — same as ota_0 — so the test partition's
  seq number actually maps to `ota_0`. The bootloader walks the OTA
  chain (`bootloader_utility_load_boot_image` from `start_index`
  backward to `FACTORY_INDEX`), and `TEST_APP_INDEX` is only entered
  when `start_index == TEST_APP_INDEX`, which is set exclusively by the
  GPIO-hold path in `bootloader_start.c`. So `test` partitions are
  GPIO-only.
- **`esp_ota_set_boot_partition(factory)` works by erasing otadata.**
  Looking at `esp_ota_ops.c`: for the factory subtype, the function
  finds the otadata partition and calls `esp_partition_erase_range()`
  on it. Next boot, the bootloader sees all-0xff otadata, and with a
  factory partition present, falls back to factory.
- **IDF auto-flashes the project bin to whichever partition `parttool.py
  --partition-boot-default` returns.** That prefers factory. We work
  around by adding a custom `esptool_py_flash_to_partition(flash
  "factory" "${nvs_dst_file}")` (last-write-wins on the duplicated
  offset in `flasher_args.json`) and an explicit
  `esptool_py_flash_to_partition(flash "ota_0" ...)` for the main app.
  The build emits `Warning: 1/3 app partitions are too small for
  binary havencore_satellite.bin` for factory — expected.

### OTA watch-items

- Binary headroom is **~540 KB per slot** (3.46 MB binary, 4 MB slot,
  ~13% free). VAD code adds little (TFLite runtime is shared with
  microWakeWord), but landing AEC + WebSocket on top of VAD could push
  the binary over. If `idf.py size` shows < 200 KB free in
  `havencore_satellite.bin`, repartition: shrink `model` to 1 MB and
  grow each app slot to ~4.5 MB.
- Version-skip uses `git describe --always --tags --dirty` on both ends
  (IDF compiles it into `esp_app_desc_t::version`; the publish script
  re-derives it). If you build twice from the same SHA with different
  uncommitted edits, both get tagged `<sha>-dirty` and the second build
  will be falsely skipped on the device. Commit the change to roll the
  hash forward when version-skip matters.
- No firmware signing yet; intentional per the trusted-LAN scope. Add
  once a satellite leaves the LAN.

Conversational follow-up window + tap-to-barge shipped 2026-04-26:
`audio_play_finish_cb` arms a no-wake-word listen window (default 5 s,
NVS key `follow_up_ms`, bounds 0–15 s) once TTS playback ends — any VAD
speech-onset within the window starts a fresh capture, silent expiry
returns to IDLE without uploading. A screen tap during SPEAKING calls
`audio_player_stop()` and starts a new turn. **Voice barge-in (speak to
interrupt playback) remains deferred** — it shares the AEC blocker with
the existing barge-in / AEC entry below.

### Follow-up onset tuning

Onset detection inside the follow-up window requires **silence-first +
N-consecutive-speech frames** (constants `FOLLOW_UP_SILENCE_FRAMES_REQ`
= 6 frames / 120 ms and `FOLLOW_UP_SPEECH_FRAMES_REQ` = 2 frames / 40 ms
in `main/app/app_sr.c`). This was needed because the first iteration
fired the trigger as soon as `simple_vad_state() == SIMPLE_VAD_SPEECH`,
which produced a runaway loop (TTS reply → arm window → instant false
trigger → empty STT → "thank you" default → TTS → …). Two pitfalls,
both worth knowing for any future "open-the-mic-after-X" feature:

- **`simple_vad_reset()` is a footgun mid-session.** It slams
  `noise_floor` back to `NOISE_INIT` (60), which drops the speech
  threshold (`noise_floor * 4`) to ~240 RMS — well below typical room
  noise on a BOX-3, so SPEECH fires on the very next frame. Original
  arming code called it to "clear stale state"; removing the call
  (preserving the adapted floor) was the actual fix. Header doc on
  `simple_vad_reset()` now warns about this.
- **Acoustic tail + I2S RX residue keeps SPEECH latched right after
  playback ends.** Even with a sane floor, the VAD often reports SPEECH
  on the first few frames of the window. Requiring N silence frames
  before honouring SPEECH filters this out without needing a hard
  delay/sleep at arm time.

If a similar feature regresses, look first at: (a) is something
resetting the noise floor when it shouldn't, and (b) does the consumer
require a true silence→speech *transition* or just current-state
SPEECH.

What's working:
- Boot, PSRAM init, LVGL start, GT911 touch init (BSP patch applied).
- Wi-Fi + DHCP + three boot health probes (`/api/status`, `/api/stt/health`,
  `/api/tts/health`) all return 200 against `selene.renman.wtf`.
- NVS reads `ssid / password / Base_url / voice / wake_enabled / device_name / session_id / listen_cap_s / silence_ms`.
- Chat traffic is on the first-party `/api/chat` endpoint (HTTP timeout 60 s). Verified 2026-04-18: full turn (wake → STT → `/api/chat` → TTS → playback) completes with `/api/chat` round-trip ≈ 1.1 s against `selene.renman.wtf`.
- `X-Session-Id` is an NVS-persisted random 32-char hex blob minted on first boot and stable across reboots (verified 2026-04-18). Server-initiated rotation via the `X-Session-Id` response header is wired (`on_session_rotated` → `settings_set_session_id`) but not yet exercised on hardware — needs an LRU eviction / server restart to force a new id.
- `build_url()` strips a trailing `/v1/` (one-shot warning) — keep it out of
  NVS to avoid the noise.
- microWakeWord loaded from the `model` SPIFFS partition. Feed task
  downmixes stereo I2S to mono and fans out to `mww_feed_pcm` +
  `simple_vad_feed` + WAV capture. Touch-to-talk and (when the override is
  reverted) NVS-gated "Hey Selene" share the same listen flow.
- Partition layout rebalanced again (2026-04-29) for OTA — `factory`
  1.5 MB (TinyUF2), `ota_0` + `ota_1` 4 MB each (main app A/B), 1 MB
  `storage`, 2 MB `model`. TinyUF2 moved into `factory` (the only
  OTA-immune software-bootable subtype) and the main app moved out of
  factory into `ota_0` to free factory for recovery.

What's shaky:
- No TLS in firmware. Server must stay on plain HTTP for the satellite, or
  we add the `mbedtls` bundle (deferred below).
- Pre-BSP BOOT-button recovery (`boot_button_held()` in `main/main.c`)
  doesn't actually trigger UF2 on a cold boot — see Known Issues.

## Known issues

### Pre-BSP BOOT-button recovery doesn't fire (2026-04-18)

`main/main.c:boot_button_held()` polls GPIO0 (BSP_BUTTON_CONFIG_IO) very
early in `app_main` and calls `settings_factory_reset()` if held ~2 s. On
hardware the screen stays black and the host sees a USB device but never
the UF2 mass-storage drive. Most likely cause: holding BOOT *through* the
power-on reset puts the ESP32-S3 into ROM download mode before our app
runs, so the poll never executes.

Settings → factory-reset already works end-to-end, so this only matters
when a device is wedged (bad Wi-Fi creds, can't reach Settings). Planned
fix: poll BOOT *after* the main app is up (e.g. while retrying Wi-Fi or
from a dedicated FreeRTOS task), not during the pre-BSP window. Low
priority — `scripts/bootstrap_factory.sh` + the esptool path cover the
"device is unrecoverable" cases until then.

### BSP touch init patch (`managed_components/` is gitignored)

`esp-box-3` 1.1.3 uses the legacy `esp_lcd_new_panel_io_i2c_v1` path, which rejects a non-zero `scl_speed_hz`. Newer `esp_lcd_touch_tt21100` macros set that field, so the BSP crashes in `bsp_touch_new()` on boot. Fix is a one-line patch that clears `tp_io_config.scl_speed_hz` before the call. Because `managed_components/` is gitignored and regenerated on `idf.py fullclean`, the patch lives under `patches/esp-box-3-scl_speed_hz.patch`. Run `./patches/apply.sh` after any re-resolve. Drop this workaround when the BSP moves to the new `i2c_master` driver.

## MVP verification checklist

Manual checkpoints for a freshly-flashed device attached via USB/IP:

1. Boot: debug overlay shows `/api/status` 200 on long-press.
2. Happy path: tap → "what time is it" → audible correct reply.
3. Tool call: "turn on the office lights" → actual light toggles.
4. Multi-turn: two turns within 180 s referencing each other.
5. Endpointing: 1.2 s silence cut-off + 15 s hard cap.
6. Error path: stop the HavenCore container → ERROR screen + auto-return.
7. Wake-word range: "Hey Selene, …" detected from 1 m and 3 m, and the
   wake phrase itself isn't included in the STT upload. **Deferred.**
8. Soak: idle 24 h, then trigger a turn — verify Wi-Fi reconnect + clean
   round-trip. **Deferred.**

## Deferred (not in MVP)

Each of these is intentionally out of scope today. Notes below describe the planned shape when we come back to them.

### Wake-word (microWakeWord)

Now integrated in-tree: `components/microwakeword/` hosts a clean-room
implementation of the microWakeWord streaming runtime (int8 TFLite +
micro_speech frontend), fed 16 kHz mono PCM from `audio_feed_task`. The
"Hey Selene" model + manifest live in `model/` and are flashed into a
dedicated 1 MB SPIFFS partition (`model` @ 0xe00000). This replaces both
the deferred-Porcupine plan *and* ESP-SR's wakenet/AFE; neither ships in
the firmware anymore.

Tunables we still need to surface: `probability_cutoff`,
`sliding_window_size`, `tensor_arena_size` come from the manifest JSON;
the Python training pipeline owns those. Nothing to do on the firmware
side unless we want to override per-device.

### Voice barge-in / AEC

Tap-to-barge shipped 2026-04-26 (see Current status). What's still
deferred is **voice** barge-in — letting the user speak to interrupt
playback. ESP-SR's AFE (which provided an AEC hook) was removed in the
microWakeWord migration, so there's no existing echo-cancel plumbing to
enable: `audio_feed_task` runs unconditionally during SPEAKING and the
mic stream is the speaker output blended with the user. We'd need to
bring AEC back in as a standalone component, re-routing the I2S TX
buffer back into it as the echo reference. Nontrivial, costs CPU, and
BOX-3 speaker loudness usually clips the mics anyway.

### SSE streaming of `/v1/chat/completions`

Today we wait for the full chat response before starting TTS (stream=false in the request body). Streaming would let us begin TTS mid-response and cut latency, but requires an SSE parser and chunked-body handling that esp_http_client doesn't give us directly. Worth doing once baseline latency is measured.

### Streaming TTS

Kokoro currently returns a full WAV from `/v1/audio/speech`. A chunked/streaming
variant would let the device begin playback before the full body arrives and
cut perceived latency by ~1–2 s. Server-side work, then a firmware switch
to parse the WAV header early and feed PCM to I2S as bytes arrive (the
playback path already does this for the full-body case).

### WebSocket `/ws/chat`

Would let the device display real-time status ("searching web…", "controlling
lights…") while the agent runs tools, instead of a generic THINKING panel.
Moderate firmware complexity (WebSocket client + event parser). Revisit once
baseline UX is stable.

### Always-on local VAD (no touch / wake gate)

`simple_vad.c` is only used to endpoint the listen window today; gating wake
on touch or microWakeWord means the device never starts capture on its own.
Flipping to always-on VAD would save the gesture/phrase but risks
false-trigger spam — not worth it until the pipeline is rock-solid.

### ~~Multi-device identity (`X-Satellite-Id` header)~~ — shipped 2026-04-18

Replaced by two headers stamped on every HavenCore request (see `components/havencore_client/havencore_client.c`): `X-Session-Id` (NVS-persisted random 32-char hex blob minted on first boot; auto-rotated via the `/api/chat` response header) and `X-Device-Name` (user-entered room label, default `Satellite`, editable in the Settings screen and persisted in NVS as `device_name`).

### TLS / auth

The trusted-LAN assumption is explicit. TLS would add an mbedtls bundle
(~80 KB) and a cert/key flow. Punt until we put a satellite outside the LAN.

## Housekeeping

- When any deferred item above lands, move its section into the relevant commit message and delete it here.
