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
#include "settings.h"
#include "state.h"

static const char *TAG = "app_sr";

#define I2S_CHANNEL_NUM      2
/* I2S read chunk: 20 ms mono at 16 kHz = 320 samples = 640 bytes mono.
 * With 2-channel I2S capture we read 1280 bytes per tick. */
#define CHUNK_SAMPLES        320

/* Listen-window tunables, now runtime-editable via settings. Defaults
 * mirror the historical compile-time constants (15 s cap, 1.2 s silence).
 * Both are read per-frame by audio_detect_task, so updates from
 * app_sr_set_*() take effect on the next LISTENING window with no
 * reboot needed. Frame cadence is 20 ms — silence_frames_cutoff is
 * silence_ms / 20. */
static uint64_t s_listen_max_us        = 15ULL * 1000ULL * 1000ULL;
static uint32_t s_silence_frames_cutoff = 60;

#define MODEL_MOUNT_POINT    "/srmodel"
#define MODEL_TFLITE_PATH    MODEL_MOUNT_POINT "/hey_selene_v1.tflite"
#define MODEL_JSON_PATH      MODEL_MOUNT_POINT "/hey_selene_v1.json"

static bool manul_detect_flag = false;
/* Conversational follow-up window: when > 0 and current time has not
 * passed it, audio_detect_task triggers a record on the next VAD
 * speech-onset *without* requiring the wake word. Set/cleared via
 * app_sr_start_follow_up_window / app_sr_cancel_follow_up_window. */
static volatile int64_t s_follow_up_deadline_us = 0;
/* Minimum silence frames observed inside the window before we'll honour
 * a SPEECH state — prevents post-playback acoustic tail / I2S RX residue
 * from instantly firing the trigger. ~6 frames * 20 ms = 120 ms. */
#define FOLLOW_UP_SILENCE_FRAMES_REQ 6
/* And require a couple of consecutive SPEECH frames so a single noisy
 * sample doesn't fire either. */
#define FOLLOW_UP_SPEECH_FRAMES_REQ  2
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
    /* Per-window follow-up tracking. Reset whenever the deadline goes
     * from 0 -> non-zero (window armed). */
    int64_t fu_last_deadline_us  = 0;
    int     fu_silence_run       = 0;
    int     fu_speech_run        = 0;

    while (true) {
        if (NEED_DELETE && xEventGroupGetBits(g_sr_data->event_group)) {
            xEventGroupSetBits(g_sr_data->event_group, DETECT_DELETED);
            vTaskDelete(g_sr_data->handle_task);
            vTaskDelete(NULL);
        }

        /* Either trigger enters LISTENING. Wake-word path is gated by
         * wake_word_enabled() exactly as before. The follow-up window
         * adds a third path: when armed, any VAD speech-onset triggers
         * a record without requiring the wake word. */
        bool triggered = false;

        if (wake_word_enabled() && mww_poll_detected()) {
            ESP_LOGI(TAG, "wake word fired");
            triggered = true;
        }
        if (manul_detect_flag) {
            manul_detect_flag = false;
            triggered = true;
        }

        /* Follow-up window: wait for a real silence -> speech transition
         * inside the window before firing. Acoustic tail from the just-
         * completed playback (or stale samples in the I2S RX buffer)
         * can leave the VAD in SPEECH at the moment we arm, so we
         * require N consecutive silence frames first, then N consecutive
         * speech frames. Window expiry without that transition returns
         * the device to IDLE without uploading. */
        if (!triggered && !detect_flag && s_follow_up_deadline_us > 0) {
            int64_t now_us = esp_timer_get_time();
            if (s_follow_up_deadline_us != fu_last_deadline_us) {
                fu_last_deadline_us = s_follow_up_deadline_us;
                fu_silence_run = 0;
                fu_speech_run  = 0;
            }
            if (now_us >= s_follow_up_deadline_us) {
                ESP_LOGI(TAG, "follow-up window expired; back to IDLE");
                s_follow_up_deadline_us = 0;
                fu_last_deadline_us = 0;
                fu_silence_run = 0;
                fu_speech_run  = 0;
                sat_state_set(SAT_STATE_IDLE);
            } else {
                simple_vad_state_t fvs = simple_vad_state();
                if (fvs == SIMPLE_VAD_SILENCE) {
                    if (fu_silence_run < FOLLOW_UP_SILENCE_FRAMES_REQ) {
                        fu_silence_run++;
                    }
                    fu_speech_run = 0;
                } else { /* SPEECH */
                    if (fu_silence_run >= FOLLOW_UP_SILENCE_FRAMES_REQ) {
                        fu_speech_run++;
                        if (fu_speech_run >= FOLLOW_UP_SPEECH_FRAMES_REQ) {
                            ESP_LOGI(TAG, "follow-up speech-onset; entering listen");
                            s_follow_up_deadline_us = 0;
                            fu_last_deadline_us = 0;
                            fu_silence_run = 0;
                            fu_speech_run  = 0;
                            triggered = true;
                        }
                    }
                    /* SPEECH before settling: hold off. */
                }
            }
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

            bool silence_cutoff  = (silence_frames >= s_silence_frames_cutoff);
            bool wallclock_cutoff = (esp_timer_get_time() - detect_start_us) > (int64_t)s_listen_max_us;

            if (silence_cutoff || wallclock_cutoff) {
                if (wallclock_cutoff && !silence_cutoff) {
                    ESP_LOGW(TAG, "listen hit %lu s wall-clock cap",
                             (unsigned long)(s_listen_max_us / 1000000ULL));
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

    /* Seed the runtime tunables from the NVS-loaded settings. Setters
     * clamp to bounds, so bad values (e.g. stale schema) don't reach
     * the detect loop. */
    sys_param_t *params = settings_get_parameter();
    app_sr_set_listen_cap_s(params->listen_cap_s);
    app_sr_set_silence_ms(params->silence_ms);


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

static uint32_t clamp_u32_local(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void app_sr_set_listen_cap_s(uint32_t seconds)
{
    uint32_t clamped = clamp_u32_local(seconds, LISTEN_CAP_S_MIN, LISTEN_CAP_S_MAX);
    s_listen_max_us = (uint64_t)clamped * 1000ULL * 1000ULL;
    ESP_LOGI(TAG, "listen cap now %lu s", (unsigned long)clamped);
}

void app_sr_set_silence_ms(uint32_t ms)
{
    uint32_t clamped = clamp_u32_local(ms, SILENCE_MS_MIN, SILENCE_MS_MAX);
    /* 20 ms per frame — see CHUNK_SAMPLES. Round to nearest frame so a
     * slider value of e.g. 1250 ms doesn't silently floor to 1240. */
    s_silence_frames_cutoff = (clamped + 10) / 20;
    ESP_LOGI(TAG, "silence cutoff now %lu ms (%lu frames)",
             (unsigned long)clamped, (unsigned long)s_silence_frames_cutoff);
}

void app_sr_start_follow_up_window(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        s_follow_up_deadline_us = 0;
        return;
    }
    /* Keep simple_vad's adapted noise floor — calling simple_vad_reset()
     * here would slam it back to NOISE_INIT (60), dropping the speech
     * threshold to ~240 RMS and letting normal room noise instantly
     * fire the trigger. The audio_detect_task follow-up branch enforces
     * silence-first + N-consecutive-speech-frames, which already filters
     * stale-state from playback bleed. */
    simple_vad_set_floor_locked(false);
    s_follow_up_deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    ESP_LOGI(TAG, "follow-up window armed for %lu ms", (unsigned long)timeout_ms);
}

void app_sr_cancel_follow_up_window(void)
{
    if (s_follow_up_deadline_us != 0) {
        ESP_LOGI(TAG, "follow-up window cancelled");
    }
    s_follow_up_deadline_us = 0;
}

bool app_sr_follow_up_active(void)
{
    return s_follow_up_deadline_us > 0
        && esp_timer_get_time() < s_follow_up_deadline_us;
}
