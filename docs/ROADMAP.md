# Roadmap

Tracks known issues, deferred work, and planned improvements. The authoritative spec remains [`../plan.md`](../plan.md); this document captures what's left on top of what's already built.

## Known issues (blocking first flash)

### `ota_0` partition too small

Current binary is ~3.5 MB; `ota_0` is 2 MB. Flashing will fail.

- Seed was built for a smaller app (no extra HTTP client code, fewer components pulled in by IDF 5.5).
- Fix candidates:
  1. Grow `ota_0` in `partitions.csv` to ~4 MB (reduces `storage`/`srmodels` headroom).
  2. Drop the UF2 factory partition for dev builds — skip the mass-storage provisioning path and write NVS via `nvs_flash` host tool instead.
  3. Strip ESP-SR models we don't use (we only need one wake-word model, not the full set).
- Before flashing, resolve this one way or the other.

## MVP verification (Phase 6 of the original plan)

Once the device is attached via USB/IP, run through [`plan.md` §Verification Plan](../plan.md) items 1–6:

1. Boot: debug overlay shows `/api/status` 200 on long-press.
2. Happy path: tap → "what time is it" → audible correct reply.
3. Tool call: "turn on the office lights" → actual light toggles.
4. Multi-turn: two turns within 180 s referencing each other.
5. Endpointing: 1.2 s silence cut-off + 15 s hard cap.
6. Error path: stop the HavenCore container → ERROR screen + auto-return.

Item 7 (wake-word) and item 8 (24 h soak) are deferred.

## Deferred (not in MVP)

Each of these is intentionally out of scope today. Notes below describe the planned shape when we come back to them.

### Wake-word (Porcupine)

`wake_word.{h,c}` is currently a runtime gate stub returning `false`. Plan: integrate Picovoice Porcupine's ESP32-S3 export once we have the `.ppn` keyword file and the Xtensa static lib. Wire it so `wake_word_enabled()` checks both NVS (`wake_enabled`) and Porcupine init success. The ESP-SR AFE pipeline already runs and already supplies VAD endpointing, so the wake-word integration only needs to replace the `res->wakeup_state == WAKENET_DETECTED` branch in `audio_detect_task`.

### 15 s hard cap on listening

Today, only VAD silence (≈1.2 s) ends a turn. If the AFE never sees silence (noisy room, continuous speech), the mic keeps recording. Plan.md specifies a 15 s hard cap — add a wall-clock check inside `audio_detect_task` or `sr_handler_task` that force-posts `ESP_MN_STATE_TIMEOUT` after 15 s of `detect_flag == true`.

### Dedicated ERROR screen with countdown

ERROR currently routes to the SLEEP panel with a 3 s auto-return. Plan.md's table asks for a dedicated screen with the error message and a visible countdown. Would mean a new SquareLine panel (`ui_PanelError`) and a corresponding case in `sat_state_set()`. Low priority — the auto-return already recovers cleanly.

### Configurable 15 s cap / silence threshold

Neither constant is exposed via NVS yet. Add as optional keys when the defaults prove wrong in practice.

### mDNS discovery of `havencore.local`

`Base_url` is stored verbatim. If the host uses `.local`, lwip mDNS resolution has to be enabled in sdkconfig. Currently we rely on the router's DNS or a static IP in the URL. Cheap add — enable `CONFIG_LWIP_USE_MDNS_RESOLVER=y` when it becomes an issue.

### Barge-in / AEC

Plan.md explicitly excludes this from MVP. `afe_config.aec_init = false` in `app_sr.c`. Enabling AEC means re-routing the TX buffer back into the AFE as the echo reference — nontrivial, costs CPU, and the playback loudness on the BOX-3 speaker usually clips the mics anyway.

### SSE streaming of `/v1/chat/completions`

Today we wait for the full chat response before starting TTS (stream=false in the request body). Streaming would let us begin TTS mid-response and cut latency, but requires an SSE parser and chunked-body handling that esp_http_client doesn't give us directly. Worth doing once baseline latency is measured.

### Multi-device identity (`X-Satellite-Id` header)

Not needed with one device. When we have two satellites, add a device-id NVS key and stamp every request.

### OTA updates

Factory + app partitions are in place but there's no update flow. Plan: pull signed OTA images from the HavenCore host itself over HTTP. Requires growing `ota_0` as mentioned above, then `esp_https_ota` (or `esp_http_ota` on the trusted LAN).

### TLS / auth

Trusted-LAN assumption is explicit in plan.md. Adds a mbedtls bundle (~80 KB) and a cert/key flow. Punt until we put a satellite outside the LAN.

## Housekeeping

- `plan.md` is the source of truth for intent; if we change direction, update it there first, then reflect in this doc.
- When any deferred item above lands, move its section into the relevant commit message and delete it here.
- Partition overflow is the only real blocker for Phase 6; everything else above is planned enhancement rather than broken behavior.
