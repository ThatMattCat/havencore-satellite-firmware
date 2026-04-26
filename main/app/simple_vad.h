/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 * SPDX-License-Identifier: CC0-1.0
 *
 * Tiny RMS-energy VAD with an adaptive noise floor. Replaces the AFE VAD
 * we used to take from ESP-SR; the only consumer is app_sr.c's listen-
 * window silence-cutoff logic.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIMPLE_VAD_SILENCE = 0,
    SIMPLE_VAD_SPEECH  = 1,
} simple_vad_state_t;

/**
 * Initialise internal state. Idempotent — but note this slams the noise
 * floor back to NOISE_INIT (60), which means the speech threshold drops
 * to ~240 RMS for ~seconds while the floor re-adapts. Call only at boot
 * (or after a long quiet period). DO NOT call during a hot session as
 * a way of "clearing" stale state — typical room noise will instantly
 * trigger SIMPLE_VAD_SPEECH at the low threshold. If you want to gate
 * onset detection, track silence-first + N-consecutive-speech frames in
 * the consumer instead (see audio_detect_task's follow-up branch).
 */
void simple_vad_reset(void);

/**
 * Feed some mono 16 kHz int16 samples. Internally accumulates into 20 ms
 * frames and updates the speech/silence state + noise-floor EMA.
 */
void simple_vad_feed(const int16_t *samples, size_t n_samples);

/**
 * Current state (updated by simple_vad_feed). Starts as SILENCE.
 */
simple_vad_state_t simple_vad_state(void);

/**
 * Freeze / unfreeze the noise-floor EMA. While frozen, the floor stays
 * at whatever value it had when the lock was applied — speech detection
 * still runs against that frozen reference. Use this during a listen
 * window so inter-word silences can't push the floor up and swallow the
 * rest of the utterance.
 */
void simple_vad_set_floor_locked(bool locked);

#ifdef __cplusplus
}
#endif
