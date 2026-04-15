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

typedef struct {
    char ssid[SSID_SIZE];             /* SSID of target AP. */
    char password[PASSWORD_SIZE];     /* Password of target AP. */
    char url[URL_SIZE];               /* HavenCore agent base URL. */
    char voice[VOICE_SIZE];           /* TTS voice id. Defaults to "af_heart". */
    uint8_t wake_enabled;             /* 0 = touch-to-talk only, 1 = wake-word armed. */
} sys_param_t;

esp_err_t settings_factory_reset(void);
esp_err_t settings_read_parameter_from_nvs(void);
sys_param_t *settings_get_parameter(void);
