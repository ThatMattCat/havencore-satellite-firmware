/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 * SPDX-License-Identifier: CC0-1.0
 *
 * TFLite Micro interpreter wrapper for microWakeWord streaming models.
 * Single-instance — we only ever run one wake-word model at a time.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/micro/micro_utils.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern "C" {
#include "microwakeword_internal.h"
}

namespace {

constexpr int kNumOps = 25;

/* Max number of VAR_HANDLE resource variables. Streaming mWW models
 * usually keep one state tensor per residual/inception block; 20 is
 * generous. Each entry is ~32 bytes of bookkeeping plus the state
 * buffer (the latter is allocated on demand inside the arena). */
constexpr int kMaxResourceVariables = 20;

const tflite::Model *s_model = nullptr;
tflite::MicroInterpreter *s_interpreter = nullptr;
tflite::MicroMutableOpResolver<kNumOps> *s_resolver = nullptr;
tflite::MicroAllocator *s_allocator = nullptr;
tflite::MicroResourceVariables *s_resource_vars = nullptr;
uint8_t *s_arena = nullptr;
uint32_t s_arena_size = 0;
TfLiteTensor *s_input = nullptr;
TfLiteTensor *s_output = nullptr;

/* microWakeWord streaming v2 models have a stride-3 first conv and take
 * (1, N, 40) where N is the number of fresh 10 ms slices consumed per
 * invocation — typically N=3, meaning one Invoke() per 30 ms of audio.
 * The model's VAR_HANDLE state tensors carry longer-range context
 * between invocations, so we must NOT feed a rolling 1-slice-at-a-time
 * window; we accumulate N fresh slices and invoke once. */
int s_input_time_slices = 1;

/* Per-invocation slice accumulator. Sized to the largest N we support
 * (3 covers every current mWW streaming model). */
constexpr int kMaxSlicesPerInvoke = 3;
int8_t  s_slice_buf[kMaxSlicesPerInvoke * MWW_FEATURES_PER_SLICE];
int     s_slices_collected = 0;

const char *TAG = "mww_model";

}  // namespace

extern "C" esp_err_t mww_model_init(const uint8_t *tflite_bytes,
                                    size_t tflite_len,
                                    uint32_t tensor_arena_size)
{
    (void)tflite_len;

    /* The JSON manifest's tensor_arena_size is the size microWakeWord
     * measured using the raw MicroInterpreter(arena, size) path. When
     * we route through MicroAllocator + MicroResourceVariables (needed
     * for the VAR_HANDLE state tensors every streaming model uses),
     * the allocator's tail overhead plus resource-variable bookkeeping
     * eats another ~8 KB. Add headroom so AllocateTensors doesn't fail
     * with "Failed to resize buffer". 16 KB is comfortably above the
     * observed ~4 KB shortfall. */
    s_arena_size = tensor_arena_size + 16 * 1024;

    /* Prefer internal SRAM (cache-coherent, faster for TFLM scratch).
     * Fall back to PSRAM if arena is huge. */
    s_arena = (uint8_t *)heap_caps_malloc(s_arena_size,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_arena) {
        ESP_LOGW(TAG, "arena: internal SRAM alloc failed, trying PSRAM");
        s_arena = (uint8_t *)heap_caps_malloc(s_arena_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_arena) {
        ESP_LOGE(TAG, "arena alloc failed (%u B)", (unsigned)s_arena_size);
        return ESP_ERR_NO_MEM;
    }

    s_model = tflite::GetModel(tflite_bytes);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "schema mismatch: model v%d vs runtime v%d",
                 (int)s_model->version(), (int)TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }

    s_resolver = new tflite::MicroMutableOpResolver<kNumOps>();
    /* Superset covering current microWakeWord inception-style v2 models
     * and older depthwise-separable v1 models. Unused ops cost nothing. */
    s_resolver->AddCallOnce();
    s_resolver->AddVarHandle();
    s_resolver->AddReadVariable();
    s_resolver->AddAssignVariable();
    s_resolver->AddConv2D();
    s_resolver->AddDepthwiseConv2D();
    s_resolver->AddFullyConnected();
    s_resolver->AddMul();
    s_resolver->AddAdd();
    s_resolver->AddMean();
    s_resolver->AddLogistic();
    s_resolver->AddQuantize();
    s_resolver->AddDequantize();
    s_resolver->AddReshape();
    s_resolver->AddStridedSlice();
    s_resolver->AddConcatenation();
    s_resolver->AddAveragePool2D();
    /* Additional ops commonly emitted by microWakeWord v2
     * (inception-style) streaming graphs. */
    s_resolver->AddSplit();
    s_resolver->AddSplitV();
    s_resolver->AddPad();
    s_resolver->AddRelu();
    s_resolver->AddLeakyRelu();
    s_resolver->AddMaxPool2D();
    s_resolver->AddMinimum();
    s_resolver->AddMaximum();

    /* Build a MicroAllocator explicitly so we can share it with a
     * MicroResourceVariables instance — required for models that use
     * VAR_HANDLE / READ_VARIABLE / ASSIGN_VARIABLE (i.e. every streaming
     * microWakeWord model). */
    s_allocator = tflite::MicroAllocator::Create(s_arena, s_arena_size);
    if (!s_allocator) {
        ESP_LOGE(TAG, "MicroAllocator::Create failed");
        mww_model_deinit();
        return ESP_FAIL;
    }
    s_resource_vars = tflite::MicroResourceVariables::Create(
        s_allocator, kMaxResourceVariables);
    if (!s_resource_vars) {
        ESP_LOGE(TAG, "MicroResourceVariables::Create failed");
        mww_model_deinit();
        return ESP_FAIL;
    }

    s_interpreter = new tflite::MicroInterpreter(
        s_model, *s_resolver, s_allocator, s_resource_vars);

    TfLiteStatus status = s_interpreter->AllocateTensors();
    if (status != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (status=%d)", (int)status);
        mww_model_deinit();
        return ESP_FAIL;
    }

    s_input  = s_interpreter->input(0);
    s_output = s_interpreter->output(0);

    /* Derive the time dimension. Expected shape is (1, N, 40) — we need
     * to feed N slices of 40 features each invocation. If the shape is
     * unexpected, fall back to 1 slice. */
    s_input_time_slices = 1;
    if (s_input && s_input->dims->size >= 2) {
        int feat_dim = s_input->dims->data[s_input->dims->size - 1];
        if (feat_dim == MWW_FEATURES_PER_SLICE &&
            s_input->dims->size == 3) {
            s_input_time_slices = s_input->dims->data[1];
        }
    }
    if (s_input_time_slices > kMaxSlicesPerInvoke) {
        ESP_LOGE(TAG, "input expects %d slices > kMaxSlicesPerInvoke=%d",
                 s_input_time_slices, kMaxSlicesPerInvoke);
        mww_model_deinit();
        return ESP_FAIL;
    }
    s_slices_collected = 0;
    ESP_LOGI(TAG, "time slices per invoke: %d", s_input_time_slices);

    /* Pre-fill the input tensor with its zero_point — that's "silence"
     * in this quantization scheme. Otherwise the first two invocations
     * (before the rolling window fills) would see a "pop" from whatever
     * garbage was in memory, poisoning the VAR_HANDLE state. */
    if (s_input) {
        int8_t *input_data = tflite::GetTensorData<int8_t>(s_input);
        size_t in_count = tflite::ElementCount(*s_input->dims);
        memset(input_data, (int)s_input->params.zero_point, in_count);
    }

    ESP_LOGI(TAG, "arena used=%u / %u B, input dims=%d, output dims=%d",
             (unsigned)s_interpreter->arena_used_bytes(),
             (unsigned)s_arena_size,
             s_input ? s_input->dims->size : -1,
             s_output ? s_output->dims->size : -1);
    /* Log tensor shapes + quantization params so we can see how to
     * interpret output[] correctly. */
    if (s_input) {
        int total = 1;
        char buf[64]; int off = 0;
        for (int i = 0; i < s_input->dims->size; ++i) {
            total *= s_input->dims->data[i];
            off += snprintf(buf + off, sizeof(buf) - off, "%s%d",
                            i == 0 ? "" : "x", s_input->dims->data[i]);
        }
        ESP_LOGI(TAG, "input:  type=%d shape=%s count=%d scale=%.6f zp=%d",
                 (int)s_input->type, buf, total,
                 s_input->params.scale, (int)s_input->params.zero_point);
    }
    if (s_output) {
        int total = 1;
        char buf[64]; int off = 0;
        for (int i = 0; i < s_output->dims->size; ++i) {
            total *= s_output->dims->data[i];
            off += snprintf(buf + off, sizeof(buf) - off, "%s%d",
                            i == 0 ? "" : "x", s_output->dims->data[i]);
        }
        ESP_LOGI(TAG, "output: type=%d shape=%s count=%d scale=%.6f zp=%d",
                 (int)s_output->type, buf, total,
                 s_output->params.scale, (int)s_output->params.zero_point);
    }

    return ESP_OK;
}

extern "C" void mww_model_deinit(void)
{
    /* s_allocator and s_resource_vars are allocated from inside s_arena
     * (they're placement-new'd onto persistent memory owned by the
     * MicroAllocator), so freeing the arena releases them too. Just
     * drop our pointers. */
    if (s_interpreter) { delete s_interpreter; s_interpreter = nullptr; }
    if (s_resolver)    { delete s_resolver;    s_resolver    = nullptr; }
    if (s_arena)       { free(s_arena);        s_arena       = nullptr; }
    s_allocator      = nullptr;
    s_resource_vars  = nullptr;
    s_model   = nullptr;
    s_input   = nullptr;
    s_output  = nullptr;
    s_slices_collected = 0;
}

extern "C" esp_err_t mww_model_invoke(
    const int8_t features[MWW_FEATURES_PER_SLICE],
    float *probability_out,
    bool  *did_invoke_out)
{
    if (!s_interpreter || !s_input || !s_output ||
        !probability_out || !did_invoke_out || !features) {
        return ESP_ERR_INVALID_STATE;
    }
    *did_invoke_out = false;

    /* Buffer this slice. */
    memcpy(s_slice_buf + s_slices_collected * MWW_FEATURES_PER_SLICE,
           features, MWW_FEATURES_PER_SLICE);
    s_slices_collected++;
    if (s_slices_collected < s_input_time_slices) {
        return ESP_OK;
    }

    /* Full window collected — copy all N slices into the input tensor in
     * chronological order (oldest at offset 0, newest at the tail, which
     * matches the (batch, time, freq) layout the stride-3 first conv
     * expects) and run one inference. */
    int8_t *input_data = tflite::GetTensorData<int8_t>(s_input);
    const size_t in_count = tflite::ElementCount(*s_input->dims);
    const size_t window_bytes =
        (size_t)s_input_time_slices * MWW_FEATURES_PER_SLICE;
    if (in_count < window_bytes) {
        s_slices_collected = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(input_data, s_slice_buf, window_bytes);
    /* Zero any trailing bytes if the flat tensor count isn't exactly N*40
     * (shouldn't happen for (1, N, 40) but be defensive). */
    if (in_count > window_bytes) {
        memset(input_data + window_bytes, 0, in_count - window_bytes);
    }
    s_slices_collected = 0;

    if (s_interpreter->Invoke() != kTfLiteOk) {
        return ESP_FAIL;
    }

    /* Output is a single quantized probability. The tensor's actual dtype
     * varies between models: some export as int8 (type=9), some as uint8
     * (type=3). Reading the wrong dtype collapses high-probability
     * outputs (e.g. uint8 255 → p≈0.996) to negative int8 values (-1)
     * which get clamped to 0 — that was silently masking every hit.
     * Promote to int32 via the correct dtype first, then dequantize. */
    int32_t raw;
    if (s_output->type == kTfLiteUInt8) {
        raw = (int32_t)tflite::GetTensorData<uint8_t>(s_output)[0];
    } else {
        raw = (int32_t)tflite::GetTensorData<int8_t>(s_output)[0];
    }
    float scale = s_output->params.scale;
    int32_t zp  = s_output->params.zero_point;
    *probability_out = ((float)raw - (float)zp) * scale;
    if (*probability_out < 0.0f) *probability_out = 0.0f;
    if (*probability_out > 1.0f) *probability_out = 1.0f;
    *did_invoke_out = true;

    /* Per-invoke diagnostic (raw output + first byte of each slice slot).
     * Re-enable if detection starts misbehaving — useful for confirming
     * all three input slots change every invoke (no staleness) and that
     * the output decode is picking the right dtype. */
#if 0
    static int dbg_counter = 0;
    if (++dbg_counter >= 33) {
        dbg_counter = 0;
        int i40 = s_input_time_slices > 1 ? MWW_FEATURES_PER_SLICE : 0;
        int i80 = s_input_time_slices > 2 ? 2 * MWW_FEATURES_PER_SLICE : 0;
        int i119 = (int)window_bytes - 1;
        ESP_LOGI(TAG,
                 "raw out[0]=%ld p=%.3f  slot0[in[0]]=%d slot1[in[40]]=%d "
                 "slot2[in[80]]=%d slot2_last[in[%d]]=%d",
                 (long)raw, *probability_out,
                 (int)input_data[0],
                 (int)input_data[i40],
                 (int)input_data[i80],
                 i119,
                 (int)input_data[i119]);
    }
#endif
    return ESP_OK;
}
