/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "app_ui_ctrl.h"
#include "havencore_client.h"
#include "audio_player.h"
#include "app_sr.h"
#include "bsp/esp-bsp.h"
#include "bsp_board.h"
#include "app_audio.h"
#include "app_wifi.h"
#include "settings.h"
#include "state.h"
#include "wake_word.h"
#include "debug_overlay.h"

#define SCROLL_START_DELAY_S            (1.5)
#define SERVER_ERROR                    "server_error"
#define INVALID_REQUEST_ERROR           "invalid_request_error"
#define SORRY_CANNOT_UNDERSTAND         "Sorry, I can't understand."
#define API_KEY_NOT_VALID               "API Key is not valid"

static char *TAG = "app_main";
static sys_param_t *sys_param = NULL;

/* Pre-BSP poll for a long-press on the BOX-3 "Config" button (GPIO0,
 * active-low). Runs before display init so a user whose Wi-Fi is
 * misconfigured can force the UF2 recovery path without navigating the
 * Settings screen. Hold through power-on for ~2 s. */
static bool boot_button_held(uint32_t hold_ms)
{
    const gpio_num_t btn = BSP_BUTTON_CONFIG_IO;
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(btn),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    if (gpio_get_level(btn) != 0) {
        return false;
    }
    const int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(btn) == 0) {
        if ((esp_timer_get_time() - t0) / 1000 >= (int64_t)hold_ms) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

/* Persist a server-rotated X-Session-Id back into NVS so subsequent boots
 * pick up the new id. Fires on the HTTP task context from havencore_chat. */
static void on_session_rotated(const char *new_id)
{
    settings_set_session_id(new_id);
}

/* Boot-time health probes. Non-blocking: we report results to the log but
 * always proceed to ready. */
static void boot_health_task(void *arg)
{
    while (wifi_connected_already() != WIFI_STATUS_CONNECTED_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    havencore_get_ok(sys_param->url, "/api/status");
    havencore_get_ok(sys_param->url, "/api/stt/health");
    havencore_get_ok(sys_param->url, "/api/tts/health");
    vTaskDelete(NULL);
}

/* One full turn: STT -> chat -> TTS -> playback. Called from app_audio.c
 * once an utterance has been captured into `audio` (WAV, 16kHz/16-bit mono).
 * Base URL for HavenCore comes from NVS (sys_param->url). */
esp_err_t start_havencore_turn(uint8_t *audio, int audio_len)
{
    esp_err_t ret = ESP_OK;
    uint8_t *tts_wav = NULL;
    size_t tts_wav_len = 0;
    FILE *fp = NULL;

    static char transcript[512];
    static char reply[2048];
    transcript[0] = '\0';
    reply[0] = '\0';

    sat_state_set(SAT_STATE_UPLOADING);

    ret = havencore_stt(sys_param->url, audio, audio_len,
                       transcript, sizeof(transcript));
    if (ret != ESP_OK || transcript[0] == '\0') {
        ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, SORRY_CANNOT_UNDERSTAND);
        debug_overlay_set_last_error("stt: empty/failed");
        sat_state_set(SAT_STATE_ERROR);
        if (ret == ESP_OK) ret = ESP_ERR_INVALID_RESPONSE;
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[stt]: no text");
    }

    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_QUESTION, transcript);
    ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, transcript);

    sat_state_set(SAT_STATE_THINKING);

    ret = havencore_chat(sys_param->url, transcript, reply, sizeof(reply));
    if (ret != ESP_OK || reply[0] == '\0') {
        ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, SORRY_CANNOT_UNDERSTAND);
        debug_overlay_set_last_error("chat: empty/failed");
        sat_state_set(SAT_STATE_ERROR);
        if (ret == ESP_OK) ret = ESP_ERR_INVALID_RESPONSE;
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[chat]: no reply");
    }

    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_CONTENT, reply);
    sat_state_set(SAT_STATE_SPEAKING);

    ret = havencore_tts(sys_param->url, sys_param->voice, reply,
                       &tts_wav, &tts_wav_len);
    if (ret != ESP_OK || tts_wav == NULL || tts_wav_len == 0) {
        debug_overlay_set_last_error("tts: empty/failed");
        sat_state_set(SAT_STATE_ERROR);
        if (ret == ESP_OK) ret = ESP_ERR_INVALID_RESPONSE;
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[tts]: no audio");
    }

    esp_err_t status = ESP_FAIL;
    fp = fmemopen((void *)tts_wav, tts_wav_len, "rb");
    if (fp) {
        status = audio_player_play(fp);
    }

    if (status != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_play failed: %s", esp_err_to_name(status));
        sat_state_set(SAT_STATE_IDLE);
    } else {
        vTaskDelay(pdMS_TO_TICKS(SCROLL_START_DELAY_S * 1000));
        ui_ctrl_reply_set_audio_start_flag(true);
    }

err:
    if (tts_wav) {
        free(tts_wav);
    }
    return ret;
}

/* play audio function */

static void audio_play_finish_cb(void)
{
    ESP_LOGI(TAG, "tts playback done");
    if (ui_ctrl_reply_get_audio_start_flag()) {
        ui_ctrl_reply_set_audio_end_flag(true);
    }
}

void app_main()
{
    /* bsp_codec_set_fs unconditionally closes both codec handles before
     * reopening, so the very first call (and any close-while-closed in
     * the BSP path) logs `E i2s_common: i2s_channel_disable(1218): the
     * channel has not been enabled yet`. The BSP doesn't track codec
     * state and the error is purely cosmetic — silence the tag. */
    esp_log_level_set("i2s_common", ESP_LOG_NONE);

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (boot_button_held(2000)) {
        ESP_LOGW(TAG, "BOOT held at startup - forcing UF2 recovery");
        esp_err_t fr = settings_factory_reset();
        ESP_LOGE(TAG, "BOOT-held factory reset did not restart: %s",
                 esp_err_to_name(fr));
        /* Fall through to normal boot; the fatal-error screen further
         * down will catch the blank-ota_0 case. */
    }

    esp_err_t settings_ret = settings_read_parameter_from_nvs();
    sys_param = settings_get_parameter();

    havencore_client_set_session_id(sys_param->session_id);
    havencore_client_set_session_changed_cb(on_session_rotated);
    havencore_client_set_device_name(sys_param->device_name);

    bsp_spiffs_mount();
    bsp_i2c_init();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = 0,
        .flags = {
            .buff_dma = true,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_board_init();

    ESP_LOGI(TAG, "Display LVGL demo");
    bsp_display_backlight_on();
    ui_ctrl_init();
    debug_overlay_init();

    if (settings_ret != ESP_OK) {
        /* NVS required keys missing AND the UF2 recovery partition is
         * unreachable (blank, corrupt, or absent). Park on a recovery
         * message — don't start the normal app flow with undefined
         * sys_param. Fix is either flashing factory_nvs.bin to ota_0 or
         * provisioning NVS directly per docs/PROVISIONING.md. */
        ESP_LOGE(TAG, "settings_read_parameter_from_nvs -> %s; parking",
                 esp_err_to_name(settings_ret));
        ui_ctrl_show_fatal_error(
            "NVS empty and UF2 recovery image missing.\n"
            "Hold BOOT during power-on, or run\n"
            "scripts/bootstrap_ota0.sh on the host.");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    sat_state_init();
    wake_word_set_enabled(sys_param->wake_enabled != 0);
    app_network_start();

    ESP_LOGI(TAG, "speech recognition start");
    app_sr_start(false);
    audio_register_play_finish_cb(audio_play_finish_cb);

    xTaskCreate(&boot_health_task, "boot_health", 4 * 1024, NULL, 3, NULL);

    while (true) {

        ESP_LOGD(TAG, "\tDescription\tInternal\tSPIRAM");
        ESP_LOGD(TAG, "Current Free Memory\t%d\t\t%d",
                 heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGD(TAG, "Min. Ever Free Size\t%d\t\t%d",
                 heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(5 * 1000));
    }
}
