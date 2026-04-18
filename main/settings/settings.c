/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "settings.h"
#include "esp_ota_ops.h"
#include "esp_random.h"

static const char *TAG = "settings";
const char *uf2_nvs_partition = "nvs";
const char *uf2_nvs_namespace = "configuration";
static nvs_handle_t my_handle;
static sys_param_t g_sys_param = {0};

esp_err_t settings_factory_reset(void)
{
    const esp_partition_t *update_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "ota_0 partition missing from table");
        return ESP_ERR_NOT_FOUND;
    }

    /* Confirm ota_0 actually holds a bootable image before flipping the
     * boot pointer. If it's blank/corrupt the bootloader will silently
     * fall back to `factory` and we'd loop forever; bail early instead
     * so main.c can surface a recovery message. */
    esp_app_desc_t desc;
    esp_err_t err = esp_ota_get_partition_description(update_partition, &desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_0 has no valid app image (%s) - run "
                 "scripts/bootstrap_ota0.sh to seed the UF2 recovery app",
                 esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(ota_0) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "switching to UF2 recovery (ota_0) and restarting");
    esp_restart();
    return ESP_OK;  /* unreachable */
}

esp_err_t settings_read_parameter_from_nvs(void)
{
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READONLY, &my_handle);
    if (ESP_ERR_NVS_NOT_FOUND == ret) {
        ESP_LOGI(TAG, "Credentials not found");
        goto err;
    }

    ESP_GOTO_ON_FALSE(ESP_OK == ret, ret, err, TAG, "nvs open failed (0x%x)", ret);
    size_t len = 0;

    // Read SSID
    len = sizeof(g_sys_param.ssid);
    ret = nvs_get_str(my_handle, "ssid", g_sys_param.ssid, &len);
    if (ret != ESP_OK || len == 0) {
        ESP_LOGI(TAG, "No SSID found");
        goto err;
    }

    // Read password
    len = sizeof(g_sys_param.password);
    ret = nvs_get_str(my_handle, "password", g_sys_param.password, &len);
    if (ret != ESP_OK || len == 0) {
        ESP_LOGI(TAG, "No Password found");
        goto err;
    }

    // Read url (HavenCore agent base URL)
    len = sizeof(g_sys_param.url);
    ret = nvs_get_str(my_handle, "Base_url", g_sys_param.url, &len);
    if (ret != ESP_OK || len == 0) {
        ESP_LOGI(TAG, "No agent base url found");
        goto err;
    }

    // Read voice (optional; defaults to af_heart if absent)
    len = sizeof(g_sys_param.voice);
    esp_err_t voice_ret = nvs_get_str(my_handle, "voice", g_sys_param.voice, &len);
    if (voice_ret != ESP_OK || len == 0) {
        strlcpy(g_sys_param.voice, "af_heart", sizeof(g_sys_param.voice));
    }

    /* wake_enabled (optional). Stored as string "0"/"1" so TinyUF2's
     * CONFIG.INI interface surfaces it for editing — the library only
     * exposes string-typed keys. Legacy u8-typed value from older
     * firmware is migrated below. Default: on. */
    char wake_str[4] = {0};
    size_t wake_len = sizeof(wake_str);
    esp_err_t wake_str_ret = nvs_get_str(my_handle, "wake_enabled",
                                         wake_str, &wake_len);
    uint8_t legacy_wake = 0;
    bool wake_needs_migration = false;
    if (wake_str_ret == ESP_OK && wake_len > 0) {
        g_sys_param.wake_enabled = (wake_str[0] == '1') ? 1 : 0;
    } else if (nvs_get_u8(my_handle, "wake_enabled", &legacy_wake) == ESP_OK) {
        g_sys_param.wake_enabled = legacy_wake ? 1 : 0;
        wake_needs_migration = true;
    } else {
        g_sys_param.wake_enabled = 1;
    }

    // Read device_name (optional; defaults to "Satellite" if absent)
    len = sizeof(g_sys_param.device_name);
    esp_err_t name_ret = nvs_get_str(my_handle, "device_name", g_sys_param.device_name, &len);
    if (name_ret != ESP_OK || len == 0) {
        strlcpy(g_sys_param.device_name, "Satellite", sizeof(g_sys_param.device_name));
    }

    // Read session_id (optional; mint a random 32-hex-char blob on first boot)
    len = sizeof(g_sys_param.session_id);
    esp_err_t sid_ret = nvs_get_str(my_handle, "session_id", g_sys_param.session_id, &len);
    bool mint_session_id = (sid_ret != ESP_OK || len == 0);

    nvs_close(my_handle);

    /* One-time NVS migrations / cleanups requiring RW access. Safe to
     * run on every boot: both operations are no-ops once the state is
     * already correct. */
    {
        nvs_handle_t rw;
        esp_err_t m_ret = nvs_open_from_partition(uf2_nvs_partition,
                                                  uf2_nvs_namespace,
                                                  NVS_READWRITE, &rw);
        if (m_ret == ESP_OK) {
            bool dirty = false;
            if (wake_needs_migration) {
                nvs_erase_key(rw, "wake_enabled");
                nvs_set_str(rw, "wake_enabled",
                            g_sys_param.wake_enabled ? "1" : "0");
                ESP_LOGI(TAG, "migrated wake_enabled u8 -> str \"%s\"",
                         g_sys_param.wake_enabled ? "1" : "0");
                dirty = true;
            }
            /* Legacy ChatGPT_key from upstream chatgpt_demo. Not read by
             * this firmware; erase so TinyUF2 stops surfacing it in
             * CONFIG.INI. */
            if (nvs_erase_key(rw, "ChatGPT_key") == ESP_OK) {
                ESP_LOGI(TAG, "erased legacy ChatGPT_key");
                dirty = true;
            }
            if (dirty) {
                nvs_commit(rw);
            }
            nvs_close(rw);
        } else {
            ESP_LOGW(TAG, "migration nvs open failed: 0x%x", m_ret);
        }
    }

    if (mint_session_id) {
        uint8_t buf[16];
        esp_fill_random(buf, sizeof(buf));
        for (int i = 0; i < 16; ++i) {
            snprintf(&g_sys_param.session_id[i * 2], 3, "%02x", buf[i]);
        }
        g_sys_param.session_id[32] = '\0';
        ESP_LOGI(TAG, "minted session_id=%s", g_sys_param.session_id);
        settings_set_session_id(g_sys_param.session_id);
    }

    ESP_LOGI(TAG, "stored ssid:%s", g_sys_param.ssid);
    ESP_LOGI(TAG, "stored password:%s", g_sys_param.password);
    ESP_LOGI(TAG, "stored Base URL:%s", g_sys_param.url);
    ESP_LOGI(TAG, "voice:%s wake_enabled:%u device_name:%s session_id:%s",
             g_sys_param.voice, g_sys_param.wake_enabled,
             g_sys_param.device_name, g_sys_param.session_id);
    return ESP_OK;

err:
    if (my_handle) {
        nvs_close(my_handle);
    }
    /* If factory reset succeeds it restarts — so a returned value here
     * always means the UF2 recovery path is unreachable (ota_0 blank or
     * corrupt). Propagate so main.c can show an on-screen recovery
     * message instead of ESP_ERROR_CHECK-crashing. */
    return settings_factory_reset();
}

sys_param_t *settings_get_parameter(void)
{
    return &g_sys_param;
}

esp_err_t settings_set_device_name(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace,
                                            NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs open (rw) failed: 0x%x", ret);
        return ret;
    }

    ret = nvs_set_str(handle, "device_name", name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str device_name failed: 0x%x", ret);
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: 0x%x", ret);
        return ret;
    }

    strlcpy(g_sys_param.device_name, name, sizeof(g_sys_param.device_name));
    ESP_LOGI(TAG, "device_name set to \"%s\"", g_sys_param.device_name);
    return ESP_OK;
}

esp_err_t settings_set_session_id(const char *id)
{
    if (!id) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace,
                                            NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs open (rw) failed: 0x%x", ret);
        return ret;
    }

    ret = nvs_set_str(handle, "session_id", id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str session_id failed: 0x%x", ret);
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: 0x%x", ret);
        return ret;
    }

    strlcpy(g_sys_param.session_id, id, sizeof(g_sys_param.session_id));
    ESP_LOGI(TAG, "session_id set to \"%s\"", g_sys_param.session_id);
    return ESP_OK;
}
