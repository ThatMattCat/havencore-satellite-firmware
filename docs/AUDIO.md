# Audio Pipeline

The audio path from BOX-3 mics through wake-word + VAD + record buffer
to TTS playback. This is the most-actively-evolving area of the
firmware — AEC, voice barge-in, and streaming TTS all sit here on the
roadmap, so the design choices below are written for the people coming
back to extend them.

Related docs:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — overall component graph and
  turn-level FSM.
- [`ROADMAP.md`](ROADMAP.md) — deferred work in this area
  (voice barge-in / AEC, streaming TTS, always-on VAD).

## Pipeline tasks

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
```

## Wake-word (microWakeWord)

`components/microwakeword/` hosts a clean-room implementation of the
microWakeWord streaming runtime: int8 TFLite + micro_speech frontend,
fed 16 kHz mono PCM. The "Hey Selene" model + manifest live in `model/`
and are flashed into a dedicated 2 MB SPIFFS partition (`model`
@ 0xa90000). This replaces both the deferred-Porcupine plan *and*
ESP-SR's wakenet/AFE; neither ships in the firmware anymore.

Wake-word is gated at the source in `audio_detect_task`:
`mww_poll_detected()` only triggers a LISTENING transition when
`wake_word_enabled()` returns true. The feed path keeps calling
`mww_feed_pcm()` unconditionally — skipping feeds while listening
would tear the streaming model's hidden state. The manual-trigger path
(touch → `manul_detect_flag`) is always on and re-uses the same
detect-flag flow, so touch-to-talk works even if `wake_word_enabled()`
returns false.

Tunables we still need to surface: `probability_cutoff`,
`sliding_window_size`, `tensor_arena_size` come from the manifest JSON;
the Python training pipeline owns those. Nothing to do on the firmware
side unless we want to override per-device.

## Endpointing (simple_vad)

`main/app/simple_vad.c` is a tiny RMS-energy VAD with adaptive noise
floor. It replaced ESP-SR's AFE VAD when microWakeWord landed. Two
roles:

1. **Listen-window endpointing** — `audio_detect_task` ends the window
   after `silence_ms` of silence following the first SPEECH frame, or
   `listen_cap_s` wall-clock, whichever fires first. Both are
   user-editable (Settings sliders, NVS keys; see
   [`SETTINGS.md`](SETTINGS.md)).
2. **Follow-up onset detection** — see below.

The noise floor adapts during silence; it's locked once a listen window
opens so that a loud first syllable doesn't move the floor up under it.

## Follow-up window

`audio_play_finish_cb` arms a no-wake-word listen window (default 5 s,
NVS key `follow_up_ms`, bounds 0–15 s) once TTS playback ends — any VAD
speech-onset within the window starts a fresh capture, silent expiry
returns to IDLE without uploading. Shipped 2026-04-26.

Tap-to-barge interacts with this: a screen tap during SPEAKING calls
`app_suppress_follow_up_once()` + `audio_player_stop()`, forcing an
immediate playback IDLE; the suppression flag prevents the follow-up
window from auto-arming since the tap is the next turn's wake. The
existing `manul_detect_flag` path takes over.

### Onset detection: silence-first + N-speech

Onset detection inside the follow-up window requires
**silence-first + N-consecutive-speech frames** (constants
`FOLLOW_UP_SILENCE_FRAMES_REQ` = 6 frames / 120 ms and
`FOLLOW_UP_SPEECH_FRAMES_REQ` = 2 frames / 40 ms in
`main/app/app_sr.c`). At least `FOLLOW_UP_SILENCE_FRAMES_REQ`
consecutive `SIMPLE_VAD_SILENCE` frames must occur before any
`SIMPLE_VAD_SPEECH` is honoured, then `FOLLOW_UP_SPEECH_FRAMES_REQ`
consecutive SPEECH frames before firing.

This shape was needed because the first iteration fired the trigger as
soon as `simple_vad_state() == SIMPLE_VAD_SPEECH`, which produced a
runaway loop: TTS reply → arm window → instant false trigger → empty
STT → "thank you" default → TTS → … Two pitfalls, both worth knowing
for any future "open-the-mic-after-X" feature:

- **`simple_vad_reset()` is a footgun mid-session.** It slams
  `noise_floor` back to `NOISE_INIT` (60), which drops the speech
  threshold (`noise_floor * 4`) to ~240 RMS — well below typical room
  noise on a BOX-3, so SPEECH fires on the very next frame. Original
  arming code called it to "clear stale state"; removing the call
  (preserving the adapted floor) was the actual fix. Header doc on
  `simple_vad_reset()` now warns about this. **Do not call it
  mid-session.**
- **Acoustic tail + I2S RX residue keeps SPEECH latched right after
  playback ends.** Even with a sane floor, the VAD often reports
  SPEECH on the first few frames of the window. Requiring N silence
  frames before honouring SPEECH filters this out without needing a
  hard delay/sleep at arm time.

If a similar feature regresses, look first at: (a) is something
resetting the noise floor when it shouldn't, and (b) does the consumer
require a true silence→speech *transition* or just current-state
SPEECH.

## Hardware notes

ESP32-S3 (dual-core Xtensa LX7, 240 MHz) on the BOX-3:

- Dual I2S MEMS mics (front-array; both channels carry the same room
  mic on the BOX-3, so we downmix to mono).
- NS4150 speaker amp driven by the ESP32-S3 I2S TX.

The BSP (`components/bsp/`) is a thin wrapper that selects between the
`espressif/esp-box`, `esp-box-3`, and `esp-box-lite` managed components
at build time based on `CONFIG_BSP_BOARD_ESP32_S3_BOX_*`.
