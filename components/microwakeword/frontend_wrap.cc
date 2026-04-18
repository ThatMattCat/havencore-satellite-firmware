/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 * SPDX-License-Identifier: CC0-1.0
 *
 * Feature extractor for microWakeWord using esp-tflite-micro's bundled
 * audio preprocessor model. In 1.3.x the old pure-C
 * tensorflow/lite/experimental/microfrontend/ library was removed; the
 * preprocessor is now itself a small TFLite graph run through TFLM with
 * the `signal/` op kernels registered.
 *
 * Contract: 30 ms of int16 PCM at 16 kHz (480 samples) -> 40 int8
 * features. microWakeWord advances the window by 10 or 20 ms (manifest-
 * controlled); that's handled by the caller in microwakeword.c.
 */

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "microwakeword_internal.h"

#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

/* Embedded preprocessor model (copied from esp-tflite-micro's
 * examples/micro_speech/main/). Header is a bare `const unsigned char`
 * definition; keep it in exactly one translation unit. */
#include "audio_preprocessor_int8_model_data.h"

static const char *TAG = "mww_frontend";

/* 30 ms @ 16 kHz. */
static constexpr int kAudioWindowSamples = 480;
static constexpr int kFeaturesPerSlice   = MWW_FEATURES_PER_SLICE;
/* Arena for the preprocessor interpreter. 16 KB is what the upstream
 * example uses; gives a little headroom on top of the ~8 KB the model
 * reports. Kept in internal SRAM. */
static constexpr size_t kPreprocArenaBytes = 16 * 1024;

namespace {

using PreprocOpResolver = tflite::MicroMutableOpResolver<18>;

const tflite::Model        *s_model = nullptr;
tflite::MicroInterpreter   *s_interpreter = nullptr;
uint8_t                    *s_arena = nullptr;
alignas(16) static uint8_t  s_op_resolver_storage[sizeof(PreprocOpResolver)];
alignas(16) static uint8_t  s_interp_storage[sizeof(tflite::MicroInterpreter)];
PreprocOpResolver          *s_resolver = nullptr;
bool                        s_inited = false;

TfLiteStatus RegisterOps(PreprocOpResolver &r)
{
    TF_LITE_ENSURE_STATUS(r.AddReshape());
    TF_LITE_ENSURE_STATUS(r.AddCast());
    TF_LITE_ENSURE_STATUS(r.AddStridedSlice());
    TF_LITE_ENSURE_STATUS(r.AddConcatenation());
    TF_LITE_ENSURE_STATUS(r.AddMul());
    TF_LITE_ENSURE_STATUS(r.AddAdd());
    TF_LITE_ENSURE_STATUS(r.AddDiv());
    TF_LITE_ENSURE_STATUS(r.AddMinimum());
    TF_LITE_ENSURE_STATUS(r.AddMaximum());
    TF_LITE_ENSURE_STATUS(r.AddWindow());
    TF_LITE_ENSURE_STATUS(r.AddFftAutoScale());
    TF_LITE_ENSURE_STATUS(r.AddRfft());
    TF_LITE_ENSURE_STATUS(r.AddEnergy());
    TF_LITE_ENSURE_STATUS(r.AddFilterBank());
    TF_LITE_ENSURE_STATUS(r.AddFilterBankSquareRoot());
    TF_LITE_ENSURE_STATUS(r.AddFilterBankSpectralSubtraction());
    TF_LITE_ENSURE_STATUS(r.AddPCAN());
    TF_LITE_ENSURE_STATUS(r.AddFilterBankLog());
    return kTfLiteOk;
}

}  // namespace

extern "C" esp_err_t mww_frontend_init(uint16_t /*feature_step_size_ms*/)
{
    if (s_inited) return ESP_OK;

    s_model = tflite::GetModel(g_audio_preprocessor_int8_tflite);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "preprocessor schema version mismatch: %lu vs %d",
                 (unsigned long)s_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }

    s_arena = (uint8_t *)heap_caps_aligned_alloc(
        16, kPreprocArenaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_arena) {
        ESP_LOGE(TAG, "arena alloc failed (%u)", (unsigned)kPreprocArenaBytes);
        return ESP_ERR_NO_MEM;
    }

    s_resolver = new (s_op_resolver_storage) PreprocOpResolver();
    if (RegisterOps(*s_resolver) != kTfLiteOk) {
        ESP_LOGE(TAG, "RegisterOps failed");
        return ESP_FAIL;
    }

    s_interpreter = new (s_interp_storage) tflite::MicroInterpreter(
        s_model, *s_resolver, s_arena, kPreprocArenaBytes);
    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "preprocessor ready (arena used=%u)",
             (unsigned)s_interpreter->arena_used_bytes());
    s_inited = true;
    return ESP_OK;
}

extern "C" void mww_frontend_deinit(void)
{
    if (!s_inited) return;
    if (s_interpreter) { s_interpreter->~MicroInterpreter(); s_interpreter = nullptr; }
    if (s_resolver)    { s_resolver->~PreprocOpResolver();   s_resolver = nullptr; }
    if (s_arena)       { heap_caps_free(s_arena);            s_arena = nullptr; }
    s_model = nullptr;
    s_inited = false;
}

extern "C" size_t mww_frontend_generate(const int16_t *pcm, size_t n_samples,
                                        int8_t features_out[MWW_FEATURES_PER_SLICE])
{
    if (!s_inited || !pcm || !features_out) return 0;
    if (n_samples < (size_t)kAudioWindowSamples) return 0;

    TfLiteTensor *input = s_interpreter->input(0);
    TfLiteTensor *output = s_interpreter->output(0);
    if (!input || !output) return 0;

    int16_t *in = tflite::GetTensorData<int16_t>(input);
    memcpy(in, pcm, kAudioWindowSamples * sizeof(int16_t));

    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "preprocessor Invoke failed");
        return 0;
    }

    const int8_t *out_data = tflite::GetTensorData<int8_t>(output);
    memcpy(features_out, out_data, kFeaturesPerSlice);
    return (size_t)kAudioWindowSamples;
}
