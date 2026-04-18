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
 * Initialise internal state. Idempotent; re-initialises the noise floor
 * on each call (useful when a fresh listen window begins).
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
