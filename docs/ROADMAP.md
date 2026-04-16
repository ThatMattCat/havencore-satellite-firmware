# Roadmap

Tracks known issues, deferred work, and planned improvements. The authoritative spec remains [`../plan.md`](../plan.md); this document captures what's left on top of what's already built.

## Current status (2026-04-15)

First successful flash + boot. Device reaches READY and runs its health checks against the HavenCore host. Pipeline itself is untested — blocked on server reachability.

What's working (observed on-device):
- Boot, PSRAM init, LVGL start, GT911 touch init (BSP patch applied).
- Wi-Fi connects to `Renman` → DHCP `10.0.0.147`.
- NVS reads `ssid / password / Base_url / voice / wake_enabled`.
- `build_url()` strips trailing `/v1/` from the stored base URL and logs a one-shot warning (as designed).
- ESP-SR AFE + wakenet load; feed/detect tasks running; wake-word gated off via `wake_enabled=0`.

What's blocked:
- All three health checks return HTTP 502 against `http://ai.renman.wtf` (old host). The HavenCore agent has moved to `selene.renman.wtf` and is now fronted by nginx. Current NVS still points at the old URL.
- Firmware has **no TLS** — `esp-tls` / cert bundle is not enabled. Server side must expose plain HTTP for the satellite, or we add TLS (deferred item below) before resuming.

Resume checklist (next session):
1. Confirm `http://selene.renman.wtf/api/status` returns 200 from the dev box.
2. Erase NVS and reprovision via UF2:
   ```
   idf.py -p /dev/ttyACM0 erase-region 0x9000 0x4000
   ```
   Reset; the device boots into the UF2 factory partition and mounts as USB mass-storage. Write `Base_url=http://selene.renman.wtf` (no `/v1/` suffix — the client appends it), plus SSID / password.
3. Reset back into the app; watch the monitor for `health /api/status -> HTTP 200`.
4. Tap the screen and speak a short phrase; expect STT → chat → TTS round-trip with audible reply.
5. Walk through `plan.md` §Verification Plan items 1–6 (see below).

## Known issues

### BSP touch init patch (`managed_components/` is gitignored)

`esp-box-3` 1.1.3 uses the legacy `esp_lcd_new_panel_io_i2c_v1` path, which rejects a non-zero `scl_speed_hz`. Newer `esp_lcd_touch_tt21100` macros set that field, so the BSP crashes in `bsp_touch_new()` on boot. Fix is a one-line patch that clears `tp_io_config.scl_speed_hz` before the call. Because `managed_components/` is gitignored and regenerated on `idf.py fullclean`, the patch lives under `patches/esp-box-3-scl_speed_hz.patch`. Run `./patches/apply.sh` after any re-resolve. Drop this workaround when the BSP moves to the new `i2c_master` driver.

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
