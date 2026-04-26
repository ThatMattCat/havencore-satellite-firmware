/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#define SSID_SIZE 32
#define PASSWORD_SIZE 64
#define URL_SIZE 64
#define VOICE_SIZE 32
#define DEVICE_NAME_SIZE 32
#define SESSION_ID_SIZE 40

#define LISTEN_CAP_S_DEFAULT 15
#define LISTEN_CAP_S_MIN     5
#define LISTEN_CAP_S_MAX     60

#define SILENCE_MS_DEFAULT 1200
#define SILENCE_MS_MIN     300
#define SILENCE_MS_MAX     3000

#define FOLLOW_UP_MS_DEFAULT 5000
#define FOLLOW_UP_MS_MIN     0
#define FOLLOW_UP_MS_MAX     15000

typedef struct {
    char ssid[SSID_SIZE];             /* SSID of target AP. */
    char password[PASSWORD_SIZE];     /* Password of target AP. */
    char url[URL_SIZE];               /* HavenCore agent base URL. */
    char voice[VOICE_SIZE];           /* TTS voice id. Defaults to "af_heart". */
    uint8_t wake_enabled;             /* 0 = touch-to-talk only, 1 = wake-word armed. */
    char device_name[DEVICE_NAME_SIZE]; /* User-visible room label. Defaults to "Satellite". */
    char session_id[SESSION_ID_SIZE]; /* Random hex blob minted on first boot; rotated by server. */
    uint32_t listen_cap_s;            /* LISTENING wall-clock cap in seconds. Bounds [5, 60]. */
    uint32_t silence_ms;              /* End-of-utterance silence in milliseconds. Bounds [300, 3000]. */
    uint32_t follow_up_ms;            /* Conversational follow-up window in ms after AI playback. 0 disables. Bounds [0, 15000]. */
} sys_param_t;

esp_err_t settings_factory_reset(void);
esp_err_t settings_read_parameter_from_nvs(void);
sys_param_t *settings_get_parameter(void);
esp_err_t settings_set_device_name(const char *name);
esp_err_t settings_set_session_id(const char *id);
esp_err_t settings_set_listen_cap_s(uint32_t seconds);
esp_err_t settings_set_silence_ms(uint32_t ms);
esp_err_t settings_set_follow_up_ms(uint32_t ms);
