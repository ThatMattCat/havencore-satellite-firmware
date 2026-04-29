# Roadmap

Tracks current status, known issues, and deferred work. MVP scope
(touch + "Hey Selene" → STT → chat → TTS → playback, with status UI
and NVS provisioning) has landed; this doc captures what's left on top.

For the engineering details behind landed work, see the topic docs:

- [`AUDIO.md`](AUDIO.md) — wake-word, VAD, follow-up window (incl. the
  onset-tuning post-mortem).
- [`OTA.md`](OTA.md) — OTA paths, partition layout, gotchas worth
  re-reading before another partition pass.

When changing direction, update this doc first.

## Current status (2026-04-29)

Both BOX-3 devices running the `updates` branch against
`http://selene.renman.wtf`. Full turn round-trip (tap → STT → chat → TTS)
has been exercised.

Recent landings:

- **2026-04-18** — UF2 factory-reset flow repaired (Settings →
  factory-reset switches boot to TinyUF2 in `factory`; mounts a USB
  drive for editing `CONFIG.INI`). Multi-device identity headers
  shipped (`X-Session-Id`, `X-Device-Name`).
- **2026-04-21** — Listen-window tunables (`listen_cap_s`,
  `silence_ms`) shipped as user-editable Settings sliders.
- **2026-04-26** — Conversational follow-up window + tap-to-barge.
  `audio_play_finish_cb` arms a no-wake-word listen window (default
  5 s, NVS key `follow_up_ms`, bounds 0–15 s) once TTS playback ends;
  silent expiry returns to IDLE without uploading. A screen tap during
  SPEAKING calls `audio_player_stop()` and starts a new turn. **Voice
  barge-in (speak to interrupt playback) remains deferred** — see the
  AEC blocker below.
- **2026-04-29** — OTA infrastructure: push (`make ota IP=<addr>` for
  the dev loop) + pull (Settings → Update Firmware for end-user
  updates) + sidecar version-skip + auto-publish via CMake post-build
  hook. Partition table re-balanced for A/B OTA. Rollback enabled
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`); `boot_health_task`
  marks the image valid after the first 200 probe. Full detail in
  [`OTA.md`](OTA.md).

What's working:

- Boot, PSRAM init, LVGL start, GT911 touch init (BSP patch applied).
- Wi-Fi + DHCP + three boot health probes (`/api/status`,
  `/api/stt/health`, `/api/tts/health`) all return 200 against
  `selene.renman.wtf`.
- NVS reads `ssid / password / Base_url / voice / wake_enabled / device_name / session_id / listen_cap_s / silence_ms / follow_up_ms`.
- Chat traffic is on the first-party `/api/chat` endpoint (HTTP timeout
  60 s). Verified 2026-04-18: full turn round-trip with `/api/chat`
  ≈ 1.1 s against `selene.renman.wtf`.
- `X-Session-Id` is an NVS-persisted random 32-char hex blob minted on
  first boot and stable across reboots (verified 2026-04-18).
  Server-initiated rotation via the `X-Session-Id` response header is
  wired (`on_session_rotated` → `settings_set_session_id`) but not yet
  exercised on hardware — needs an LRU eviction / server restart to
  force a new id.
- `build_url()` strips a trailing `/v1/` (one-shot warning) — keep it
  out of NVS to avoid the noise.
- microWakeWord loaded from the `model` SPIFFS partition. Feed task
  downmixes stereo I2S to mono and fans out to `mww_feed_pcm` +
  `simple_vad_feed` + WAV capture. Touch-to-talk and "Hey Selene"
  share the same listen flow.
- OTA dev loop and end-user pull both exercised end-to-end.

What's shaky:

- No TLS in firmware. Server must stay on plain HTTP for the satellite,
  or we add the `mbedtls` bundle (deferred below).
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

`esp-box-3` 1.1.3 uses the legacy `esp_lcd_new_panel_io_i2c_v1` path,
which rejects a non-zero `scl_speed_hz`. Newer `esp_lcd_touch_tt21100`
macros set that field, so the BSP crashes in `bsp_touch_new()` on boot.
Fix is a one-line patch that clears `tp_io_config.scl_speed_hz` before
the call. Because `managed_components/` is gitignored and regenerated
on `idf.py fullclean`, the patch lives under
`patches/esp-box-3-scl_speed_hz.patch`. Run `./patches/apply.sh` after
any re-resolve. Drop this workaround when the BSP moves to the new
`i2c_master` driver.

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

Each of these is intentionally out of scope today. Notes below describe
the planned shape when we come back to them.

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

Today we wait for the full chat response before starting TTS
(`stream=false` in the request body). Streaming would let us begin TTS
mid-response and cut latency, but requires an SSE parser and
chunked-body handling that `esp_http_client` doesn't give us directly.
Worth doing once baseline latency is measured.

### Streaming TTS

Kokoro currently returns a full WAV from `/v1/audio/speech`. A
chunked/streaming variant would let the device begin playback before
the full body arrives and cut perceived latency by ~1–2 s. Server-side
work, then a firmware switch to parse the WAV header early and feed
PCM to I2S as bytes arrive (the playback path already does this for
the full-body case).

### WebSocket `/ws/chat`

Would let the device display real-time status ("searching web…",
"controlling lights…") while the agent runs tools, instead of a
generic THINKING panel. Moderate firmware complexity (WebSocket client
+ event parser). Revisit once baseline UX is stable.

### Always-on local VAD (no touch / wake gate)

`simple_vad.c` is only used to endpoint the listen window today;
gating wake on touch or microWakeWord means the device never starts
capture on its own. Flipping to always-on VAD would save the
gesture/phrase but risks false-trigger spam — not worth it until the
pipeline is rock-solid.

### TLS / auth

The trusted-LAN assumption is explicit. TLS would add an mbedtls
bundle (~80 KB) and a cert/key flow. Punt until we put a satellite
outside the LAN.

### Per-device microWakeWord tunables

`probability_cutoff`, `sliding_window_size`, and `tensor_arena_size`
come from the manifest JSON; the Python training pipeline owns those.
Nothing to do on the firmware side unless we want to override
per-device.

## Housekeeping

- When any deferred item above lands, move its section into the
  relevant commit message and delete it here. If the implementation
  carries a non-obvious lesson, capture it in the relevant topic doc
  (e.g. AUDIO.md, OTA.md) — not here.
