/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * microWakeWord: on-device streaming wake-word detection.
 * Clean-room reimplementation of the runtime contract documented at
 * https://github.com/kahrendt/microWakeWord. Trained int8 TFLite streaming
 * models are fed 40 int8 features / 10 ms produced by TFLM's micro_speech
 * front-end over 16 kHz mono int16 PCM.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load a model + manifest from the filesystem, set up the frontend and the
 * TFLite interpreter. Must be called before mww_start / mww_feed_pcm.
 *
 * @param tflite_path  Absolute path to the .tflite streaming model.
 * @param json_path    Absolute path to the companion microWakeWord manifest
 *                     JSON. probability_cutoff, sliding_window_size,
 *                     tensor_arena_size, feature_step_size are consumed.
 */
esp_err_t mww_init(const char *tflite_path, const char *json_path);

/**
 * Feed 16 kHz mono int16 PCM. May be called from any task; internally
 * queues into a ring buffer consumed by the inference task.
 *
 * @return ESP_OK on enqueue, ESP_ERR_NO_MEM if the ring buffer overflowed
 *         (the oldest samples are dropped).
 */
esp_err_t mww_feed_pcm(const int16_t *samples, size_t n_samples);

/**
 * Non-blocking poll. Returns true at most once per wake-word utterance —
 * internally clears the "detected" latch before returning. Intended to be
 * polled from `audio_detect_task` at ~100 Hz.
 */
bool mww_poll_detected(void);

/**
 * Release all resources. Safe to call even if mww_init failed.
 */
void mww_deinit(void);

#ifdef __cplusplus
}
#endif
