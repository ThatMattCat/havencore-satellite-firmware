HavenCore Satellite — ESP32-S3-BOX-3 MVP Spec

 Context

 HavenCore is a self-hosted voice assistant ("Selene") running as Docker microservices on a Linux host with NVIDIA GPUs. Previous satellites were
 Raspberry Pis running Porcupine wake-word + a Python client against the agent's OpenAI-compatible endpoints.

 We are now building a purpose-built hardware satellite on the Espressif ESP32-S3-BOX-3 (dual I2S MEMS mics, integrated speaker amp, 320×240 SPI
 touch display, 16 MB PSRAM, Wi-Fi). The BOX-3 already ships with an official Espressif chatgpt_demo reference app that does mic → OpenAI Whisper
 → OpenAI chat → OpenAI TTS → speaker — the HavenCore agent's /v1/* endpoints were designed to be drop-in compatible with that shape, so the port
 is mostly swapping hostnames and tweaking audio formats.

 This spec targets a minimum viable product: a user walks up to the box, triggers it (touch or wake-word), speaks, hears Selene reply, and sees
 state on the small screen. Tool-calling and conversation history are handled server-side by the agent — the firmware just brokers audio.

 This document is meant to be handed off to a fresh repo + conversation to drive firmware implementation. It is self-contained: no reader should
 need to open HavenCore source to start.

 ---
 Goals (MVP)

 1. End-to-end voice loop: press button (v0) or say "Hey Selene" (v1) → capture speech → round-trip HavenCore → play response from the BOX-3
 speaker.
 2. Status UI on the 320×240 screen: idle / listening / thinking / speaking / error.
 3. Wi-Fi provisioning and persistent config for agent base URL.
 4. Robust enough for daily use in one room; not yet a fleet/multi-device product.

 Non-goals (v1)

 - Barge-in / interruption while Selene is speaking.
 - Local VAD-driven always-on listening (we'll use wake-word or touch only).
 - Acoustic echo cancellation beyond whatever the BOX-3 BSP provides by default.
 - Multi-satellite coordination, OTA fleet management, encryption (assume trusted LAN).
 - Camera, IMU, or BOX-3 DOCK/SENSOR expansions.

 ---
 Chosen Stack

 ┌────────────┬────────────────────────────────┬─────────────────────────────────────────────────────────────────────────────────────────────┐
 │  Decision  │             Choice             │                                           Reason                                            │
 ├────────────┼────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────┤
 │ SoC /      │ ESP32-S3-BOX-3                 │ User's existing hardware; well-supported BSP                                                │
 │ board      │                                │                                                                                             │
 ├────────────┼────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────┤
 │ Framework  │ ESP-IDF (C/C++)                │ Matches Espressif's chatgpt_demo reference; maximum flexibility                             │
 ├────────────┼────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────┤
 │ UI library │ LVGL (via BSP)                 │ Already used by chatgpt_demo; simple state screens                                          │
 ├────────────┼────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────┤
 │            │ microWakeWord (streaming       │ Open-source, trainable in Colab, runs on ESP32-S3 via TFLite-Micro. Replaced the earlier    │
 │ Wake word  │ int8 TFLite) + touch-to-talk   │ Porcupine plan after the trained "Hey Selene" model produced a clean hit rate on-device.    │
 │            │ fallback                       │ Touch-to-talk always works as a fallback regardless of model state.                         │
 ├────────────┼────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────┤
 │ Transport  │ REST chain: STT → chat → TTS   │ Matches chatgpt_demo; simplest firmware; ~3–8s end-to-end is acceptable                     │
 │            │ over HTTP                      │                                                                                             │
 ├────────────┼────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────┤
 │ Audio      │ 16 kHz / 16-bit / mono PCM     │ Whisper-native; no resampling on server                                                     │
 │ capture    │ from I2S mic 0                 │                                                                                             │
 ├────────────┼────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────┤
 │ Audio      │ WAV decoded and streamed to    │ Server returns 16 kHz WAV regardless of requested format                                    │
 │ playback   │ I2S speaker                    │                                                                                             │
 └────────────┴────────────────────────────────┴─────────────────────────────────────────────────────────────────────────────────────────────┘

 Reference projects to study first

 - Espressif chatgpt_demo — https://github.com/espressif/esp-box/tree/master/examples/chatgpt_demo (the template we're adapting)
 - microWakeWord — https://github.com/kahrendt/microWakeWord (the streaming int8 runtime + training pipeline we're using)
 - Home Assistant ESP32-S3-BOX voice assistant (ESPHome/Wyoming) — reference for mic I2S configuration, display layouts, BSP usage. Do not adopt
 its Wyoming transport.
 - Willow (HeyWillow) — optional reference for AEC/VAD pipeline if we hit echo problems in v2.

 ---
 HavenCore API Contract

 All endpoints live on the HavenCore host. Default base URL (user-configurable):

 http://<havencore-host>/           # nginx gateway, port 80

 The three endpoints the satellite chains per turn — all OpenAI-compatible, no auth on LAN:

 1. Speech-to-Text

 POST /v1/audio/transcriptions
 Content-Type: multipart/form-data

 Fields:
 - file — WAV (16 kHz, 16-bit PCM, mono recommended)
 - model — any string, ignored (e.g. "whisper-1")
 - language — optional, e.g. "en"

 Response (200):
 { "text": "turn on the kitchen lights" }

 Backend: Faster Whisper (distil-large-v3 default). Typical latency 0.5–2 s.

 2. Agent chat (LLM + tool execution)

 POST /v1/chat/completions
 Content-Type: application/json

 Body:
 {
   "model": "selene",
   "messages": [
     { "role": "user", "content": "<STT output>" }
   ],
   "stream": false
 }

 Response (200): standard OpenAI chat.completion object. The assistant's reply is choices[0].message.content.

 Important server-side behaviors the firmware should know:

 - The agent maintains conversation history server-side keyed by session (180 s idle timeout). Sending only the current user turn is correct and
 sufficient — do not try to rebuild history on the device.
 - Tool calls (Home Assistant, web search, memory, MQTT, etc.) happen transparently inside this call. No tool loop on the client. Latency is 2–5 s
  typical, can spike higher for multi-tool turns.
 - Streaming (stream: true) is supported via SSE; MVP should use non-streaming for simplicity.

 3. Text-to-Speech

 POST /v1/audio/speech
 Content-Type: application/json

 Body:
 {
   "input": "<assistant reply text>",
   "model": "tts-1",
   "voice": "af_heart",
   "response_format": "wav"
 }

 Response (200): raw binary audio, always WAV 16 kHz mono regardless of response_format (server quirk — do not rely on the format field). Backend
 is Kokoro TTS. Typical latency 0.5–2 s for a sentence.

 Health checks

 Before going "ready", the firmware should ping:
 - GET /api/status — JSON, confirms agent + MCP + DB up
 - GET /api/tts/health and GET /api/stt/health — cheap liveness

 Latency budget (LAN, typical turn)

 ┌────────────────────────────────┬───────────┐
 │             Stage              │  Typical  │
 ├────────────────────────────────┼───────────┤
 │ Capture + encode WAV on device │ 0.2–0.5 s │
 ├────────────────────────────────┼───────────┤
 │ Upload + STT                   │ 0.5–2 s   │
 ├────────────────────────────────┼───────────┤
 │ Agent + tools                  │ 2–5 s     │
 ├────────────────────────────────┼───────────┤
 │ TTS + download                 │ 0.5–2 s   │
 ├────────────────────────────────┼───────────┤
 │ Decode + playback start        │ <0.2 s    │
 ├────────────────────────────────┼───────────┤
 │ Total to first audio           │ 3.5–10 s  │
 └────────────────────────────────┴───────────┘

 Show a "thinking" state on screen during the dead air between capture-end and TTS-start, or the experience will feel broken.

 ---
 Firmware Architecture

 Single ESP-IDF application, derived from chatgpt_demo. Suggested module layout:

 main/
   app_main.c              // init, Wi-Fi, BSP, dispatch
   config.{h,c}            // NVS-backed: agent base URL, voice, wake-word enable
   wifi_provision.{h,c}    // SoftAP or BLE provisioning on first boot
   state.{h,c}             // FSM: IDLE → LISTENING → UPLOADING → THINKING → SPEAKING → IDLE / ERROR
   audio_capture.{h,c}     // I2S mic -> ring buffer; endpointing (silence timeout)
   audio_playback.{h,c}    // WAV stream -> I2S speaker
   http_client.{h,c}       // multipart upload, JSON POST, binary GET; shared TLS/keep-alive
   pipeline.{h,c}          // orchestrates STT -> chat -> TTS per turn
   wake_word.{h,c}         // Porcupine init + detection task (stub if model absent)
   touch_trigger.{h,c}     // capacitive button / screen tap -> start listening
   ui.{h,c}                // LVGL screens for each state, error toasts

 State machine

 IDLE ──(touch or wake-word)──> LISTENING
 LISTENING ──(silence >1.2s or max 15s)──> UPLOADING
 UPLOADING ──(STT ok)──> THINKING
 UPLOADING ──(STT fail)──> ERROR
 THINKING ──(chat ok, text received)──> SPEAKING
 THINKING ──(chat fail/timeout)──> ERROR
 SPEAKING ──(playback done)──> IDLE
 ERROR ──(timeout 3s)──> IDLE

 - Any press of the touch button while in SPEAKING/THINKING = cancel back to IDLE (simple "give up" — not true barge-in).
 - LISTENING endpointing: fixed 1.2 s trailing silence OR 15 s hard cap. No fancy VAD in v1 — use amplitude threshold on the incoming I2S stream.

 Audio capture details

 - I2S RX from the BSP mic (mic 0 only; ignore mic 1 for MVP — no beamforming).
 - Sample 16 kHz / 16-bit / mono. BSP default may be 48 kHz stereo — downsample in-place or configure I2S directly.
 - Stream into a PSRAM ring buffer sized for 15 s + headroom.
 - At endpoint: prepend a minimal WAV header (RIFF / fmt  / data) and upload. Do not try to stream chunked to STT — the server is
 request/response.

 Audio playback details

 - HTTP GET returns WAV; parse the header to get sample rate (expect 16 kHz).
 - Feed PCM frames to I2S TX via DMA as the HTTP body streams in. Do not buffer the full WAV in RAM before starting — start playback after ~200 ms
  of audio to hide download latency.
 - Stop and flush cleanly on cancel.

 Wake word integration

 Rollout landed in two stages:

 v0 — touch-to-talk only:
 - Capacitive button press OR tap anywhere on the LVGL idle screen starts LISTENING.
 - wake_word.c was a no-op stub returning false.

 v1 — microWakeWord "Hey Selene" (current):
 - components/microwakeword/ hosts a clean-room runtime against the streaming int8 TFLite + micro_speech frontend contract.
 - Model (hey_selene_v1.tflite) and manifest live in model/ and flash into a dedicated 1 MB SPIFFS partition ("model" @ 0xe00000).
 - audio_feed_task downmixes stereo I2S to mono once and fans the mono stream into mww_feed_pcm + simple_vad + WAV capture.
 - audio_detect_task polls mww_poll_detected() at 20 ms; wake_word_enabled() gates whether that triggers a LISTENING transition.
 - Touch-to-talk still works regardless of model state — the manual path is independent.
 - Retraining is out of tree (Python notebook against microWakeWord's pipeline); drop a new .tflite/.json into model/ to update.

 UI (LVGL)

 Five screens, all full-screen, high contrast:

 ┌───────────┬────────────────────────────────────────────────────────────────┐
 │   State   │                             Visual                             │
 ├───────────┼────────────────────────────────────────────────────────────────┤
 │ IDLE      │ "Selene" wordmark + small "tap to talk" hint                   │
 ├───────────┼────────────────────────────────────────────────────────────────┤
 │ LISTENING │ Animated mic icon, live VU meter bar                           │
 ├───────────┼────────────────────────────────────────────────────────────────┤
 │ THINKING  │ Spinner + "thinking…" (optional: last transcript small at top) │
 ├───────────┼────────────────────────────────────────────────────────────────┤
 │ SPEAKING  │ Waveform / equalizer animation + response text if short        │
 ├───────────┼────────────────────────────────────────────────────────────────┤
 │ ERROR     │ Error text + auto-return countdown                             │
 └───────────┴────────────────────────────────────────────────────────────────┘

 Debug overlay (hold button 2 s): last HTTP error, Wi-Fi RSSI, agent base URL, free PSRAM.

 Configuration (NVS)

 Stored in flash, editable via SoftAP config portal on first boot or long-press reset:

 - agent_base_url (default http://havencore.local)
 - voice (default af_heart)
 - wake_enabled (default true; set to 0 in NVS to force touch-to-talk only)
 - wifi_ssid / wifi_pass

 Error handling

 - HTTP 5xx or timeout on any leg → ERROR screen with the stage that failed; don't hang.
 - DNS/connectivity failure on boot → retry 3× with backoff, then show "no network".
 - On STT returning empty text → short chime + return to IDLE (nothing to send).

 ---
 Verification Plan

 Each of these is a manual checkpoint, not an automated test — wire tests are out of scope for MVP firmware.

 1. Bring-up. Flash firmware, complete Wi-Fi provisioning, confirm /api/status returns 200 in debug overlay.
 2. Touch-to-talk happy path. Tap screen, say "what time is it", verify Selene answers audibly and correctly. Check latency feels like the 3–8 s
 budget.
 3. Tool-calling turn. Ask "turn on the office lights" — verify the light actually toggles (proves the /v1/chat/completions leg ran the HA MCP
 tool).
 4. Multi-turn context. Two consecutive turns within 180 s referring to each other ("what's the weather?" → "and tomorrow?") — verify server-side
 session works.
 5. Endpointing. Confirm 1.2 s silence cuts off capture; confirm 15 s cap prevents runaway.
 6. Error paths. Stop the agent container, trigger a turn, verify ERROR screen + return to IDLE.
 7. Wake-word (post-v1). With Porcupine model flashed, say "Hey Selene, …" from 1 m and 3 m; verify detection and that the wake phrase itself
 isn't included in the STT upload.
 8. Soak. Leave idle for 24 h, trigger a turn — verify Wi-Fi reconnect + clean turn.

 ---
 Open Questions for v2+

 Not blocking MVP, but worth capturing now:

 - Streaming TTS. Kokoro server currently returns full WAV. Adding a chunked streaming endpoint server-side would cut perceived latency by ~1–2 s.
 - WebSocket /ws/chat. Would let the BOX-3 display "searching web…", "controlling lights…" in real time. Moderate firmware complexity; consider
 once MVP is stable.
 - Barge-in. Requires AEC (the speaker bleeds into the mic) — Willow's pipeline is the reference.
 - Device identity / multi-room. Send a X-Satellite-Id header and have the agent scope session + routing per device.
 - Local VAD. Replace touch/wake gating with always-on VAD once we trust the pipeline; saves a gesture.
 - mDNS discovery. Auto-find havencore.local instead of requiring a typed URL.

 ---
 Hand-off notes

 When starting the new repo:

 1. Clone Espressif esp-box and copy examples/chatgpt_demo as the seed.
 2. Rip out the OpenAI URL/key plumbing and wire in agent_base_url from NVS.
 3. Replace any OpenAI-specific request shaping with the three contracts in the HavenCore API Contract section above (note the TTS "always WAV"
 quirk and the "send only current user turn" rule).
 4. Land touch-to-talk + full state machine first. Add Porcupine as a separate commit once the Picovoice Console export is in hand.
 5. Keep the Wi-Fi provisioning + NVS config out of scope creep — use whatever the BSP offers.

