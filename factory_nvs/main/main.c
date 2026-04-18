/* SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_tinyuf2.h"
#include "ui.h"
#include "bsp/esp-bsp.h"
#include "esp_ota_ops.h"

#define NVS_MODIFIED_BIT          BIT0
#define SSID_SIZE 32
#define PASSWORD_SIZE 64
#define URL_SIZE 64
#define WAKE_ENABLED_SIZE 4

static const char *TAG = "factory_nvs";

static EventGroupHandle_t s_event_group;
static nvs_handle_t my_handle;
static size_t buf_len_long;

static void uf2_nvs_modified_cb()
{
    ESP_LOGI(TAG, "uf2 nvs modified");
    xEventGroupSetBits(s_event_group, NVS_MODIFIED_BIT);
}

void app_main(void)
{
    const esp_partition_t *update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    ESP_LOGI(TAG, "Switch to partition factory");
    esp_ota_set_boot_partition(update_partition);
    esp_err_t err = ESP_OK;

    const char *uf2_nvs_partition = "nvs";
    const char *uf2_nvs_namespace = "configuration";

    char ssid[SSID_SIZE] = {0};
    char password[PASSWORD_SIZE] = {0};
    char url[URL_SIZE] = {0};
    char wake_enabled[WAKE_ENABLED_SIZE] = {0};

    s_event_group = xEventGroupCreate();

    // Initialize NVS
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        buf_len_long = sizeof(ssid);
        err = nvs_get_str(my_handle, "ssid", ssid, &buf_len_long);
        if (err != ESP_OK || buf_len_long == 0) {
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "ssid", CONFIG_ESP_WIFI_SSID));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "no ssid, give a init value to nvs");
        } else {
            ESP_LOGI(TAG, "stored ssid:%s", ssid);
        }

        buf_len_long = sizeof(password);
        err = nvs_get_str(my_handle, "password", password, &buf_len_long);
        if (err != ESP_OK || buf_len_long == 0) {
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "password", CONFIG_ESP_WIFI_PASSWORD));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "no password, give a init value to nvs");
        } else {
            ESP_LOGI(TAG, "stored password:%s", password);
        }

        buf_len_long = sizeof(url);
        err = nvs_get_str(my_handle, "Base_url", url, &buf_len_long);
        if (err != ESP_OK || buf_len_long == 0) {
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "Base_url", CONFIG_HAVENCORE_BASE_URL));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "no base url, give a init value to key");
        } else {
            ESP_LOGI(TAG, "stored base url:%s", url);
        }

        /* wake_enabled as string so TinyUF2's CONFIG.INI surfaces it for
         * editing. If a legacy u8-typed key is present, the main app's
         * settings_read_parameter_from_nvs() migrates it to a string on
         * next boot; leaving the type-mismatch case alone here. */
        buf_len_long = sizeof(wake_enabled);
        err = nvs_get_str(my_handle, "wake_enabled", wake_enabled, &buf_len_long);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "wake_enabled", "1"));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "no wake_enabled, seeded str \"1\"");
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG, "stored wake_enabled:%s", wake_enabled);
        } else {
            ESP_LOGW(TAG, "wake_enabled read err=%s (main app will migrate)",
                     esp_err_to_name(err));
        }
    }
    nvs_close(my_handle);

    /* install UF2 NVS */
    tinyuf2_nvs_config_t nvs_config = DEFAULT_TINYUF2_NVS_CONFIG();
    nvs_config.part_name = uf2_nvs_partition;
    nvs_config.namespace_name = uf2_nvs_namespace;
    nvs_config.modified_cb = uf2_nvs_modified_cb;

    ESP_ERROR_CHECK(esp_tinyuf2_install(NULL, &nvs_config));

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
    bsp_display_backlight_on();
    ui_init();

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_event_group, NVS_MODIFIED_BIT,
                                               pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & NVS_MODIFIED_BIT) {
            esp_err_t err = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READONLY, &my_handle);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open NVS partition: %s", esp_err_to_name(err));
                // Handle the error or take appropriate action
                return;
            }

            size_t buf_len_long = sizeof(ssid);
            err = nvs_get_str(my_handle, "ssid", ssid, &buf_len_long);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read 'ssid' from NVS: %s", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }
            ESP_LOGD(TAG, "SSID %s", ssid);

            buf_len_long = sizeof(password);
            err = nvs_get_str(my_handle, "password", password, &buf_len_long);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read 'password' from NVS: %s", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }
            ESP_LOGD(TAG, "Password %s", password);

            buf_len_long = sizeof(url);
            err = nvs_get_str(my_handle, "Base_url", url, &buf_len_long);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read 'Base_url' from NVS: %s", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }
            ESP_LOGD(TAG, "Base URL %s", url);
            nvs_close(my_handle);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}
