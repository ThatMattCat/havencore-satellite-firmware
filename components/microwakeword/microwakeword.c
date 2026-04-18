/*
 * SPDX-FileCopyrightText: 2026 HavenCore
 * SPDX-License-Identifier: CC0-1.0
 *
 * microWakeWord detector: orchestrates PCM -> features -> inference ->
 * sliding-window probability smoothing -> detection latch.
 *
 *   ┌──────────────┐  feed_pcm   ┌─────────────┐     inference task
 *   │ caller task  │────────────▶│ ring buffer │────────────┐
 *   └──────────────┘             └─────────────┘            ▼
 *                                                ┌──────────────────────┐
 *                                                │ frontend -> model    │
 *                                                │ -> sliding window ▶  │
 *                                                │    detected_latch    │
 *                                                └──────────────────────┘
 *   poll_detected() ◀─────────────────────────────────────────┘
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "microwakeword.h"
#include "microwakeword_internal.h"

static const char *TAG = "mww";

/* Ring buffer: 500 ms at 16 kHz mono = 16 KB. Overflow -> drop oldest. */
#define PCM_RING_BYTES (MWW_SAMPLE_RATE_HZ * sizeof(int16_t) / 2)

/* Cool-down after a detection before another can fire. Prevents a single
 * utterance latching twice if the sliding window stays above the cutoff
 * for a beat longer. */
#define DETECT_COOLDOWN_MS 2000

typedef struct {
    bool           inited;
    mww_manifest_t manifest;

    uint8_t       *tflite_bytes;   /* kept alive for interpreter lifetime */
    size_t         tflite_len;

    RingbufHandle_t pcm_ring;
    TaskHandle_t    task;
    volatile bool   stop_req;

    SemaphoreHandle_t mtx;
    bool              detected_latch;
    int64_t           last_detect_us;

    /* Sliding window of recent probabilities. */
    float    *probs;
    uint16_t  probs_cap;
    uint16_t  probs_head;
    uint16_t  probs_count;

    /* Consumed samples per inference step (step_size_ms * 16). */
    uint16_t  samples_per_step;
} mww_ctx_t;

static mww_ctx_t s_ctx;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static esp_err_t load_tflite_bytes(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen(%s) failed", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }
    /* TFLM only needs read access but the bytes must live past init. */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
    if (!buf) { fclose(fp); return ESP_ERR_NO_MEM; }
    if ((long)fread(buf, 1, sz, fp) != sz) {
        free(buf);
        fclose(fp);
        return ESP_FAIL;
    }
    fclose(fp);
    *out = buf;
    *out_len = (size_t)sz;
    return ESP_OK;
}

/* Rolling mean over the sliding window. */
static float rolling_mean(void)
{
    if (s_ctx.probs_count == 0) return 0.0f;
    float sum = 0.0f;
    for (uint16_t i = 0; i < s_ctx.probs_count; ++i) {
        sum += s_ctx.probs[i];
    }
    return sum / (float)s_ctx.probs_count;
}

static void window_push(float p)
{
    s_ctx.probs[s_ctx.probs_head] = p;
    s_ctx.probs_head = (s_ctx.probs_head + 1) % s_ctx.probs_cap;
    if (s_ctx.probs_count < s_ctx.probs_cap) s_ctx.probs_count++;
}

static void window_reset(void)
{
    s_ctx.probs_head = 0;
    s_ctx.probs_count = 0;
}

/* Window: 30 ms (480 samples). Stride: step_size (10 or 20 ms). Each
 * inference step shifts the window left by step_size samples, appends
 * step_size new samples from the ring buffer, and feeds the full 480 to
 * the preprocessor to produce one slice of 40 int8 features. */
#define WINDOW_SAMPLES 480

static void inference_task(void *arg)
{
    const size_t step_samples = s_ctx.samples_per_step;
    const size_t step_bytes = step_samples * sizeof(int16_t);
    int16_t *window = (int16_t *)heap_caps_calloc(
        WINDOW_SAMPLES, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *incoming = (int16_t *)heap_caps_malloc(
        step_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!window || !incoming) {
        ESP_LOGE(TAG, "window/incoming alloc failed");
        if (window)   free(window);
        if (incoming) free(incoming);
        vTaskDelete(NULL);
        return;
    }

    int8_t features[MWW_FEATURES_PER_SLICE];
    size_t pending = 0;
    size_t warmup_samples = 0;  /* discard first window's worth of output */

    /* Diagnostic peak/mean log — re-enable if detection starts missing
     * or false-triggering. Counted in model invocations (≈30 ms each),
     * so 33 → ~1 s log cadence. */
#if 0
    float   diag_peak_p = 0.0f;
    int     diag_ticks  = 0;
    const int diag_period_steps = 33;
#endif

    while (!s_ctx.stop_req) {
        /* Pull enough samples for one step. */
        size_t got = 0;
        void *item = xRingbufferReceiveUpTo(s_ctx.pcm_ring, &got,
            pdMS_TO_TICKS(100), step_bytes - pending);
        if (!item) continue;

        memcpy((uint8_t *)incoming + pending, item, got);
        vRingbufferReturnItem(s_ctx.pcm_ring, item);
        pending += got;
        if (pending < step_bytes) continue;
        pending = 0;

        /* Slide the window left by step_samples, append incoming. */
        memmove(window, window + step_samples,
                (WINDOW_SAMPLES - step_samples) * sizeof(int16_t));
        memcpy(window + (WINDOW_SAMPLES - step_samples), incoming, step_bytes);

        /* Skip inference until the window has been fully primed. */
        if (warmup_samples < WINDOW_SAMPLES) {
            warmup_samples += step_samples;
            continue;
        }

        size_t consumed = mww_frontend_generate(window, WINDOW_SAMPLES, features);
        if (consumed == 0) continue;

        float p = 0.0f;
        bool  did_invoke = false;
        if (mww_model_invoke(features, &p, &did_invoke) != ESP_OK) {
            ESP_LOGW(TAG, "invoke failed");
            continue;
        }
        /* Streaming v2 models run inference once per N feature slices
         * (stride-3 for the current model). Until the model actually
         * produced a probability, skip probs-window updates. */
        if (!did_invoke) continue;
        window_push(p);

#if 0
        if (p > diag_peak_p) diag_peak_p = p;
        if (++diag_ticks >= diag_period_steps) {
            ESP_LOGI(TAG, "score peak=%.3f  mean=%.3f  cutoff=%.3f",
                     diag_peak_p, rolling_mean(),
                     s_ctx.manifest.probability_cutoff);
            diag_peak_p = 0.0f;
            diag_ticks = 0;
        }
#endif

        if (s_ctx.probs_count < s_ctx.probs_cap) continue;  /* warmup */

        float mean_p = rolling_mean();
        int64_t t = now_us();
        int64_t since_last = t - s_ctx.last_detect_us;
        bool in_cooldown = since_last < ((int64_t)DETECT_COOLDOWN_MS * 1000LL);

        if (mean_p >= s_ctx.manifest.probability_cutoff && !in_cooldown) {
            ESP_LOGI(TAG, "detected '%s' (mean p=%.3f)",
                     s_ctx.manifest.wake_word, mean_p);
            xSemaphoreTake(s_ctx.mtx, portMAX_DELAY);
            s_ctx.detected_latch = true;
            s_ctx.last_detect_us = t;
            xSemaphoreGive(s_ctx.mtx);
            window_reset();
        }
    }

    free(window);
    free(incoming);
    vTaskDelete(NULL);
}

esp_err_t mww_init(const char *tflite_path, const char *json_path)
{
    if (s_ctx.inited) return ESP_ERR_INVALID_STATE;
    memset(&s_ctx, 0, sizeof(s_ctx));

    esp_err_t ret = mww_manifest_load(json_path, &s_ctx.manifest);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "manifest load failed"); return ret; }

    ret = load_tflite_bytes(tflite_path, &s_ctx.tflite_bytes, &s_ctx.tflite_len);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "tflite load failed"); return ret; }
    ESP_LOGI(TAG, "model bytes=%u", (unsigned)s_ctx.tflite_len);

    ret = mww_frontend_init(s_ctx.manifest.feature_step_size_ms);
    if (ret != ESP_OK) { free(s_ctx.tflite_bytes); return ret; }

    ret = mww_model_init(s_ctx.tflite_bytes, s_ctx.tflite_len,
                         s_ctx.manifest.tensor_arena_size);
    if (ret != ESP_OK) {
        mww_frontend_deinit();
        free(s_ctx.tflite_bytes);
        return ret;
    }

    s_ctx.samples_per_step = MWW_PCM_SAMPLES_PER_MS * s_ctx.manifest.feature_step_size_ms;

    s_ctx.probs_cap = s_ctx.manifest.sliding_window_size;
    if (s_ctx.probs_cap < 1) s_ctx.probs_cap = 1;
    s_ctx.probs = (float *)calloc(s_ctx.probs_cap, sizeof(float));
    if (!s_ctx.probs) return ESP_ERR_NO_MEM;

    s_ctx.pcm_ring = xRingbufferCreate(PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!s_ctx.pcm_ring) return ESP_ERR_NO_MEM;

    s_ctx.mtx = xSemaphoreCreateMutex();
    if (!s_ctx.mtx) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreatePinnedToCore(
        inference_task, "mww_inf", 6 * 1024, NULL, 5, &s_ctx.task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }

    s_ctx.inited = true;
    ESP_LOGI(TAG, "ready: step=%u samples, window=%u slices",
             (unsigned)s_ctx.samples_per_step, (unsigned)s_ctx.probs_cap);
    return ESP_OK;
}

esp_err_t mww_feed_pcm(const int16_t *samples, size_t n_samples)
{
    if (!s_ctx.inited || !samples || n_samples == 0) return ESP_ERR_INVALID_ARG;
    size_t bytes = n_samples * sizeof(int16_t);
    BaseType_t ok = xRingbufferSend(s_ctx.pcm_ring, samples, bytes, 0);
    if (ok != pdTRUE) {
        /* Drop the oldest slice worth of data to make room. */
        size_t drop = 0;
        void *item = xRingbufferReceiveUpTo(s_ctx.pcm_ring, &drop, 0, bytes);
        if (item) vRingbufferReturnItem(s_ctx.pcm_ring, item);
        ok = xRingbufferSend(s_ctx.pcm_ring, samples, bytes, 0);
        if (ok != pdTRUE) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool mww_poll_detected(void)
{
    if (!s_ctx.inited) return false;
    bool hit = false;
    xSemaphoreTake(s_ctx.mtx, portMAX_DELAY);
    if (s_ctx.detected_latch) {
        hit = true;
        s_ctx.detected_latch = false;
    }
    xSemaphoreGive(s_ctx.mtx);
    return hit;
}

void mww_deinit(void)
{
    if (!s_ctx.inited) return;
    s_ctx.stop_req = true;
    if (s_ctx.task) {
        /* Inference task self-deletes when stop_req is seen. Give it a
         * moment. */
        vTaskDelay(pdMS_TO_TICKS(200));
        s_ctx.task = NULL;
    }
    if (s_ctx.pcm_ring) { vRingbufferDelete(s_ctx.pcm_ring); s_ctx.pcm_ring = NULL; }
    if (s_ctx.mtx)      { vSemaphoreDelete(s_ctx.mtx);       s_ctx.mtx = NULL; }
    if (s_ctx.probs)    { free(s_ctx.probs);                 s_ctx.probs = NULL; }
    mww_model_deinit();
    mww_frontend_deinit();
    if (s_ctx.tflite_bytes) { free(s_ctx.tflite_bytes); s_ctx.tflite_bytes = NULL; }
    s_ctx.inited = false;
}
