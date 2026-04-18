# Roadmap

Tracks known issues, deferred work, and planned improvements. MVP scope
(touch + "Hey Selene" → STT → chat → TTS → playback, with status UI and
NVS provisioning) has landed; this doc captures what's left on top.

## Current status (2026-04-18)

Device #1 running the microWakeWord-migration branch against
`http://selene.renman.wtf`. Full turn round-trip (tap → STT → chat → TTS)
has been exercised. Device #2 provisioned today via the `esptool` NVS
workaround (see `docs/PROVISIONING.md`).

What's working:
- Boot, PSRAM init, LVGL start, GT911 touch init (BSP patch applied).
- Wi-Fi + DHCP + three boot health probes (`/api/status`, `/api/stt/health`,
  `/api/tts/health`) all return 200 against `selene.renman.wtf`.
- NVS reads `ssid / password / Base_url / voice / wake_enabled / device_name / session_id`.
- Chat traffic is on the first-party `/api/chat` endpoint (HTTP timeout 60 s). Verified 2026-04-18: full turn (wake → STT → `/api/chat` → TTS → playback) completes with `/api/chat` round-trip ≈ 1.1 s against `selene.renman.wtf`.
- `X-Session-Id` is an NVS-persisted random 32-char hex blob minted on first boot and stable across reboots (verified 2026-04-18). Server-initiated rotation via the `X-Session-Id` response header is wired (`on_session_rotated` → `settings_set_session_id`) but not yet exercised on hardware — needs an LRU eviction / server restart to force a new id.
- `build_url()` strips a trailing `/v1/` (one-shot warning) — keep it out of
  NVS to avoid the noise.
- microWakeWord loaded from the `model` SPIFFS partition. Feed task
  downmixes stereo I2S to mono and fans out to `mww_feed_pcm` +
  `simple_vad_feed` + WAV capture. Touch-to-talk and (when the override is
  reverted) NVS-gated "Hey Selene" share the same listen flow.
- Partition layout rebalanced for the migration — old `ota_0` 2 MB
  overflow is no longer a blocker.

What's shaky:
- `wake_word_enabled()` is hardcoded to `true` (see note below).
- No TLS in firmware. Server must stay on plain HTTP for the satellite, or
  we add the `mbedtls` bundle (deferred below).
- UF2 factory-reset flow is broken — fresh devices need the `esptool`
  provisioning path.

## Known issues

### UF2 factory-reset flow broken (2026-04-18)

`settings_factory_reset()` does `esp_ota_set_boot_partition(ota_0)` +
restart, but the device comes back into the main `factory` app instead of
TinyUF2 mass-storage. Also, fresh BOX-3 units ship with `chatgpt_demo`
placeholder NVS values (`My Network SSID` / `10.0.0.134/v1/`), so the
"missing keys" branch of `settings_read_parameter_from_nvs()` doesn't fire
in the first place — the device loops on Wi-Fi retry until you overwrite
NVS manually.

Workaround: provision via `esptool` — see [`PROVISIONING.md`](PROVISIONING.md).
Root cause is probably one of (a) `ota_0` doesn't actually contain the
TinyUF2 factory app on these devices, (b) `esp_ota_set_boot_partition`
succeeds but the bootloader falls back to `factory` because `ota_0`'s
image header is invalid, or (c) the factory_nvs sub-project needs a
rebuild + separate `write_flash 0x700000` pass. Worth confirming the next
time we touch the factory_nvs sub-project.

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

**2026-04-18 — `wake_word_enabled()` override.** `main/app/wake_word.c`
hardcodes `wake_word_enabled()` to `true` and ignores
`wake_word_set_enabled()`. Why: deployed devices still have NVS
`wake_enabled=0` from the old default, and the UF2 factory-reset flow
(see Known Issues) can't currently flip the key. Revert to the
NVS-driven implementation (see git blame on `wake_word.c`) once UF2
provisioning works again, or once we're confident every satellite has
been re-provisioned via the `esptool` path.

Tunables we still need to surface: `probability_cutoff`,
`sliding_window_size`, `tensor_arena_size` come from the manifest JSON;
the Python training pipeline owns those. Nothing to do on the firmware
side unless we want to override per-device.

### Configurable 15 s cap / silence threshold

Neither constant is exposed via NVS yet. Add as optional keys when the defaults prove wrong in practice.

### mDNS discovery of `havencore.local`

`Base_url` is stored verbatim. If the host uses `.local`, lwip mDNS resolution has to be enabled in sdkconfig. Currently we rely on the router's DNS or a static IP in the URL. Cheap add — enable `CONFIG_LWIP_USE_MDNS_RESOLVER=y` when it becomes an issue.

### Barge-in / AEC

Explicitly out of MVP scope. ESP-SR's AFE (which provided an
AEC hook) was removed in the microWakeWord migration, so there's no
existing echo-cancel plumbing to enable — we'd need to bring AEC back in
as a standalone component, re-routing the I2S TX buffer back into it as
the echo reference. Nontrivial, costs CPU, and BOX-3 speaker loudness
usually clips the mics anyway.

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

### OTA updates

Factory + app partitions are in place but there's no update flow. Plan: pull signed OTA images from the HavenCore host itself over HTTP. Requires growing `ota_0` as mentioned above, then `esp_https_ota` (or `esp_http_ota` on the trusted LAN).

### TLS / auth

The trusted-LAN assumption is explicit. TLS would add an mbedtls bundle
(~80 KB) and a cert/key flow. Punt until we put a satellite outside the LAN.

## Housekeeping

- When any deferred item above lands, move its section into the relevant commit message and delete it here.
- The only blocker for a clean first-time-install UX is the UF2 factory-reset flow. Until that's fixed, new devices need the `esptool` path in `docs/PROVISIONING.md`.
