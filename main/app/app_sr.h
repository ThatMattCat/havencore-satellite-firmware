/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 HavenCore
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * The original file imported types from ESP-SR (esp_afe_sr_models.h /
 * esp_mn_models.h). After switching the wake-word detector to
 * microWakeWord, ESP-SR is no longer a dependency; we redefine the small
 * set of enum values consumers (app_audio.c sr_handler_task) still need,
 * so those consumers stay untouched.
 */

#pragma once

#include <stdbool.h>
#include <sys/queue.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEED_DELETE          BIT0
#define FEED_DELETED         BIT1
#define DETECT_DELETED       BIT2
#define HANDLE_DELETED       BIT3

/* Event tokens emitted by audio_detect_task into result_que. Names /
 * values kept stable so app_audio.c:sr_handler_task works unchanged. */
typedef enum {
    WAKENET_NO_DETECT = 0,
    WAKENET_DETECTED  = 1,
} wakenet_state_t;

typedef enum {
    ESP_MN_STATE_DETECTING = 0,
    ESP_MN_STATE_DETECTED  = 1,  /* unused in this firmware, kept for API */
    ESP_MN_STATE_TIMEOUT   = 2,
} esp_mn_state_t;

typedef struct {
    wakenet_state_t wakenet_mode;
    esp_mn_state_t  state;
    int             command_id;
} sr_result_t;

typedef struct {
    TaskHandle_t      feed_task;
    TaskHandle_t      detect_task;
    TaskHandle_t      handle_task;
    QueueHandle_t     result_que;
    EventGroupHandle_t event_group;
} sr_data_t;

esp_err_t app_sr_start(bool record_en);
esp_err_t app_sr_stop(void);
esp_err_t app_sr_get_result(sr_result_t *result, TickType_t xTicksToWait);
esp_err_t app_sr_start_once(void);

/* Live-update hooks for the listen-window tunables. Values are clamped
 * internally; pass the user-facing units (seconds, milliseconds). The
 * change takes effect on the next LISTENING frame — no reboot needed. */
void app_sr_set_listen_cap_s(uint32_t seconds);
void app_sr_set_silence_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
