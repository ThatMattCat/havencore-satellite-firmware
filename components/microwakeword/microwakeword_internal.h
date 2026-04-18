/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 * SPDX-License-Identifier: CC0-1.0
 *
 * Internal types shared between microwakeword.c / manifest.c /
 * frontend_wrap.c / streaming_model.cc. Not a public API.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed by the microWakeWord feature contract. */
#define MWW_SAMPLE_RATE_HZ      16000
#define MWW_FEATURES_PER_SLICE  40
#define MWW_FEATURE_WINDOW_MS   30
#define MWW_PCM_SAMPLES_PER_MS  (MWW_SAMPLE_RATE_HZ / 1000)  /* 16 */

typedef struct {
    float    probability_cutoff;   /* e.g. 0.5 */
    uint16_t sliding_window_size;  /* e.g. 5 */
    uint32_t tensor_arena_size;    /* bytes */
    uint16_t feature_step_size_ms; /* 10 (v2) or 20 (v1) */
    uint16_t version;              /* manifest `version` field */
    char     wake_word[64];        /* for logging */
} mww_manifest_t;

/* --- manifest.c --- */
esp_err_t mww_manifest_load(const char *json_path, mww_manifest_t *out);

/* --- frontend_wrap.c ---
 *
 * One-shot init of the micro_speech front-end. Must be called with the
 * manifest in hand so we can configure step_size correctly.
 */
esp_err_t mww_frontend_init(uint16_t feature_step_size_ms);
void      mww_frontend_deinit(void);

/**
 * Process up to MWW_PCM_SAMPLES_PER_MS * feature_step_size_ms samples.
 * Writes `MWW_FEATURES_PER_SLICE` int8 features into `features_out`.
 * Returns the number of input samples actually consumed (so the caller
 * can advance its ring buffer read pointer).
 */
size_t mww_frontend_generate(const int16_t *pcm, size_t n_samples,
                             int8_t features_out[MWW_FEATURES_PER_SLICE]);

/* --- streaming_model.cc --- */
esp_err_t mww_model_init(const uint8_t *tflite_bytes, size_t tflite_len,
                         uint32_t tensor_arena_size);
void      mww_model_deinit(void);

/**
 * Consumes one 40-feature slice. microWakeWord v2 streaming models have
 * a stride-3 first conv and expect inference every 30 ms with three fresh
 * 10 ms slices per call; between calls the model's own VAR_HANDLE state
 * carries longer-range context. We therefore buffer slices internally and
 * only invoke the interpreter once N (= s_input_time_slices) slices have
 * arrived.
 *
 *   *did_invoke_out == false  -> still accumulating; *probability_out is
 *                                untouched. Not an error.
 *   *did_invoke_out == true   -> Invoke() ran; *probability_out is the
 *                                dequantized wake-word probability.
 */
esp_err_t mww_model_invoke(const int8_t features[MWW_FEATURES_PER_SLICE],
                           float *probability_out,
                           bool  *did_invoke_out);

#ifdef __cplusplus
}
#endif
