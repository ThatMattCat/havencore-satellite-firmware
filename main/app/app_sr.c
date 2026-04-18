/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 HavenCore
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Audio pipeline wiring:
 *
 *   I2S (stereo 16 kHz) ─ downmix ─┬─▶ mww_feed_pcm()  (wake-word detector)
 *                                  ├─▶ simple_vad_feed() (silence cutoff)
 *                                  └─▶ audio_record_save() (WAV capture for STT)
 *
 * The external contract (sr_result_t queue, manul_detect_flag path,
 * result state codes) matches the pre-migration ESP-SR code so
 * app_audio.c:sr_handler_task keeps working unchanged.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"

#include "app_sr.h"
#include "bsp_board.h"
#include "app_audio.h"
#include "app_wifi.h"
#include "wake_word.h"
#include "simple_vad.h"
#include "microwakeword.h"

static const char *TAG = "app_sr";

#define I2S_CHANNEL_NUM      2
/* I2S read chunk: 20 ms mono at 16 kHz = 320 samples = 640 bytes mono.
 * With 2-channel I2S capture we read 1280 bytes per tick. */
#define CHUNK_SAMPLES        320

/* Hard cap on how long we stay in LISTENING before force-ending the turn.
 * VAD silence normally ends things at ~1.2 s — but a continuously noisy
 * room can hold vad_state at SPEECH indefinitely, which would trap us in
 * the LISTENING panel with the mic open. */
#define LISTEN_MAX_US        (15LL * 1000LL * 1000LL)

/* Consecutive 20 ms silence frames before cutoff. 60 * 20 ms = 1.2 s,
 * matches the behaviour we had with AFE VAD + frame_keep=100. */
#define SILENCE_FRAMES_CUTOFF 60

#define MODEL_MOUNT_POINT    "/srmodel"
#define MODEL_TFLITE_PATH    MODEL_MOUNT_POINT "/hey_selene_v1.tflite"
#define MODEL_JSON_PATH      MODEL_MOUNT_POINT "/hey_selene_v1.json"

static bool manul_detect_flag = false;
sr_data_t *g_sr_data = NULL;

extern bool record_flag;
extern uint32_t record_total_len;

static esp_err_t mount_model_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path      = MODEL_MOUNT_POINT,
        .partition_label = "model",
        .max_files      = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* already mounted — fine */
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount model partition failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* Feed task: runs on core 0. Pulls I2S stereo, downmixes left-channel to
 * mono, fans it out to the wake-word detector, the VAD, and the record
 * buffer. */
static void audio_feed_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Feed Task: %d-sample chunks, mono 16 kHz", CHUNK_SAMPLES);

    const size_t stereo_bytes = CHUNK_SAMPLES * I2S_CHANNEL_NUM * sizeof(int16_t);
    int16_t *stereo = heap_caps_malloc(stereo_bytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *mono   = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(stereo && mono);

    while (true) {
        if (g_sr_data->event_group && xEventGroupGetBits(g_sr_data->event_group)) {
            xEventGroupSetBits(g_sr_data->event_group, FEED_DELETED);
            heap_caps_free(stereo);
            heap_caps_free(mono);
            vTaskDelete(NULL);
        }

        size_t bytes_read = 0;
        bsp_i2s_read((char *)stereo, stereo_bytes, &bytes_read, portMAX_DELAY);
        if (bytes_read < stereo_bytes) continue;

        /* Stereo → mono. On BOX-3 both ES7210 channels carry the same
         * room mic content (confirmed with per-channel RMS logging); we
         * take the right slot as the canonical mono. */
        for (int i = 0; i < CHUNK_SAMPLES; ++i) {
            mono[i] = stereo[i * 2 + 1];
        }

        /* Always feed the wake-word detector (cheap; the poll side is
         * gated by wake_word_enabled). Skipping it while listening would
         * tear the streaming-model hidden state. */
        mww_feed_pcm(mono, CHUNK_SAMPLES);

        /* VAD only matters during a listen window, but feeding it always
         * lets the noise floor adapt. */
        simple_vad_feed(mono, CHUNK_SAMPLES);

        /* WAV capture for STT upload. */
        audio_record_save(mono, CHUNK_SAMPLES);
    }
}

/* Detect task: runs on core 1. Emits sr_result_t events. */
static void audio_detect_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Detection task");

    bool    detect_flag    = false;
    int64_t detect_start_us = 0;
    int     silence_frames = 0;
    bool    seen_speech    = false;

    while (true) {
        if (NEED_DELETE && xEventGroupGetBits(g_sr_data->event_group)) {
            xEventGroupSetBits(g_sr_data->event_group, DETECT_DELETED);
            vTaskDelete(g_sr_data->handle_task);
            vTaskDelete(NULL);
        }

        /* Either trigger enters LISTENING. Wake-word path is gated by
         * wake_word_enabled() exactly as before. */
        bool triggered = false;

        if (wake_word_enabled() && mww_poll_detected()) {
            ESP_LOGI(TAG, "wake word fired");
            triggered = true;
        }
        if (manul_detect_flag) {
            manul_detect_flag = false;
            triggered = true;
        }

        if (triggered && !detect_flag) {
            sr_result_t result = {
                .wakenet_mode = WAKENET_DETECTED,
                .state        = ESP_MN_STATE_DETECTING,
                .command_id   = 0,
            };
            xQueueSend(g_sr_data->result_que, &result, 0);
            detect_flag    = true;
            detect_start_us = esp_timer_get_time();
            silence_frames  = 0;
            seen_speech     = false;
            /* Freeze the noise floor at whatever idle-room value it just
             * settled to — inter-word silences during speech would
             * otherwise inflate it and swallow the rest of the utterance. */
            simple_vad_set_floor_locked(true);
        }

        /* Listen-window endpointing. Poll the VAD at its own 20 ms
         * frame cadence (it updates state only when a full frame has
         * been accumulated). Two rules:
         *   - Don't start the silence timer until we've observed at
         *     least one SPEECH frame — otherwise a slightly-late talker
         *     gets cut off before they start.
         *   - After that, SILENCE_FRAMES_CUTOFF consecutive silence
         *     frames (1.2 s) end the turn.
         * Wall-clock cap (LISTEN_MAX_US, 15 s) always applies. */
        if (detect_flag) {
            simple_vad_state_t vs = simple_vad_state();
            if (vs == SIMPLE_VAD_SPEECH) {
                seen_speech = true;
                silence_frames = 0;
            } else if (seen_speech) {
                silence_frames++;
            }

            bool silence_cutoff  = (silence_frames >= SILENCE_FRAMES_CUTOFF);
            bool wallclock_cutoff = (esp_timer_get_time() - detect_start_us) > LISTEN_MAX_US;

            if (silence_cutoff || wallclock_cutoff) {
                if (wallclock_cutoff && !silence_cutoff) {
                    ESP_LOGW(TAG, "listen hit %d s wall-clock cap",
                             (int)(LISTEN_MAX_US / 1000000));
                }
                sr_result_t result = {
                    .wakenet_mode = WAKENET_NO_DETECT,
                    .state        = ESP_MN_STATE_TIMEOUT,
                    .command_id   = 0,
                };
                xQueueSend(g_sr_data->result_que, &result, 0);
                detect_flag = false;
                silence_frames = 0;
                /* Let the noise floor adapt again during idle. */
                simple_vad_set_floor_locked(false);
            }
        }

        /* Poll cadence matches the VAD's 20 ms frame update so
         * silence_frames counts once per new frame. */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t app_sr_start(bool record_en)
{
    (void)record_en;
    ESP_RETURN_ON_FALSE(NULL == g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR already running");

    g_sr_data = heap_caps_calloc(1, sizeof(sr_data_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_NO_MEM, TAG, "Failed create sr data");

    esp_err_t ret = ESP_OK;

    g_sr_data->result_que = xQueueCreate(3, sizeof(sr_result_t));
    ESP_GOTO_ON_FALSE(NULL != g_sr_data->result_que, ESP_ERR_NO_MEM, err, TAG, "result_que");

    g_sr_data->event_group = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(NULL != g_sr_data->event_group, ESP_ERR_NO_MEM, err, TAG, "event_group");

    ret = mount_model_spiffs();
    if (ret != ESP_OK) goto err;

    /* Initialise the wake-word detector. Failure here shouldn't keep
     * touch-to-talk from working, so we log and continue. */
    ret = mww_init(MODEL_TFLITE_PATH, MODEL_JSON_PATH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "microwakeword disabled (%s); touch-to-talk still active",
                 esp_err_to_name(ret));
        /* Force the wake gate off so audio_detect_task won't poll. */
        wake_word_set_enabled(false);
        ret = ESP_OK;
    }

    simple_vad_reset();

    BaseType_t ok = xTaskCreatePinnedToCore(&audio_feed_task, "Feed Task",
        6 * 1024, NULL, 5, &g_sr_data->feed_task, 0);
    ESP_GOTO_ON_FALSE(pdPASS == ok, ESP_FAIL, err, TAG, "feed task");

    ok = xTaskCreatePinnedToCore(&audio_detect_task, "Detect Task",
        6 * 1024, NULL, 5, &g_sr_data->detect_task, 1);
    ESP_GOTO_ON_FALSE(pdPASS == ok, ESP_FAIL, err, TAG, "detect task");

    ok = xTaskCreatePinnedToCore(&sr_handler_task, "SR Handler Task",
        8 * 1024, NULL, 5, &g_sr_data->handle_task, 0);
    ESP_GOTO_ON_FALSE(pdPASS == ok, ESP_FAIL, err, TAG, "handler task");

    audio_record_init();
    return ESP_OK;
err:
    app_sr_stop();
    return ret;
}

esp_err_t app_sr_stop(void)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    xEventGroupSetBits(g_sr_data->event_group, NEED_DELETE);
    xEventGroupWaitBits(g_sr_data->event_group,
        NEED_DELETE | FEED_DELETED | DETECT_DELETED | HANDLE_DELETED,
        1, 1, portMAX_DELAY);

    if (g_sr_data->result_que)  { vQueueDelete(g_sr_data->result_que);  g_sr_data->result_que = NULL; }
    if (g_sr_data->event_group) { vEventGroupDelete(g_sr_data->event_group); g_sr_data->event_group = NULL; }

    mww_deinit();
    heap_caps_free(g_sr_data);
    g_sr_data = NULL;
    return ESP_OK;
}

esp_err_t app_sr_get_result(sr_result_t *result, TickType_t xTicksToWait)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    xQueueReceive(g_sr_data->result_que, result, xTicksToWait);
    return ESP_OK;
}

esp_err_t app_sr_start_once(void)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    manul_detect_flag = true;
    return ESP_OK;
}
