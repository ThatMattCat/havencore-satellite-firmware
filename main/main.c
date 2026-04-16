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

/* Boot-time health probes. Non-blocking: we report results to the log but
 * always proceed to ready. Plan.md §Verification Plan item 1. */
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
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(settings_read_parameter_from_nvs());
    sys_param = settings_get_parameter();

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
