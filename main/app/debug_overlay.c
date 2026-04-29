/*
 * SPDX-FileCopyrightText: 2025 HavenCore
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "debug_overlay.h"
#include "state.h"
#include "settings.h"
#include "ui/ui.h"
#include "lvgl.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "bsp/esp-bsp.h"
#include <stdio.h>
#include <string.h>

#define DEBUG_OVERLAY_AUTO_HIDE_MS 15000

static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_label = NULL;
static lv_timer_t *s_timer = NULL;
static lv_timer_t *s_auto_hide = NULL;
static char s_last_error[96] = "none";

static void refresh_text(void)
{
    if (!s_label) return;

    sys_param_t *p = settings_get_parameter();
    wifi_ap_record_t ap = {0};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    /* Device IP — handy when SSH'ing in to push an OTA: `make ota IP=…`. */
    char ip_str[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    const esp_app_desc_t *desc = esp_app_get_description();

    char buf[384];
    snprintf(buf, sizeof(buf),
             "state: %s\nfw: %s\nip: %s\nurl: %s\nrssi: %d dBm\nfree psram: %u KB\nlast err: %s",
             sat_state_name(sat_state_get()),
             desc ? desc->version : "?",
             ip_str,
             p ? p->url : "?",
             rssi,
             (unsigned)(free_psram / 1024),
             s_last_error);
    lv_label_set_text(s_label, buf);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_text();
}

static void hide_overlay(void)
{
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_timer) lv_timer_pause(s_timer);
    if (s_auto_hide) lv_timer_pause(s_auto_hide);
}

static void auto_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_panel && !lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        hide_overlay();
    }
}

static void toggle_overlay(void)
{
    if (lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        refresh_text();
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_timer) lv_timer_resume(s_timer);
        /* Re-arm the auto-hide each time the overlay is shown so the
         * 15 s window restarts on every long-press, not just the first. */
        if (s_auto_hide) {
            lv_timer_reset(s_auto_hide);
            lv_timer_resume(s_auto_hide);
        }
    } else {
        hide_overlay();
    }
}

static void long_press_cb(lv_event_t *e)
{
    (void)e;
    toggle_overlay();
}

void debug_overlay_init(void)
{
    bsp_display_lock(0);

    /* Parent to the display's top layer so the overlay floats above any
     * SquareLine screen (Sleep/Listen/Get/Reply are separate screens, and
     * parenting to lv_scr_act() would bind us to whichever one is active
     * at init time). */
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, 280, 180);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_80, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_pad_all(s_panel, 8, 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_label = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_label, 264);
    lv_label_set_text(s_label, "");

    /* Register long-press on every panel we actually load via
     * lv_disp_load_scr(). Attaching to lv_scr_act() at init would only
     * cover the initial screen. */
    lv_obj_t *panels[] = {
        ui_PanelSleep, ui_PanelListen, ui_PanelGet, ui_PanelReply,
    };
    for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        if (panels[i]) {
            lv_obj_add_event_cb(panels[i], long_press_cb,
                                LV_EVENT_LONG_PRESSED, NULL);
        }
    }

    s_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);
    lv_timer_pause(s_timer);

    s_auto_hide = lv_timer_create(auto_hide_cb, DEBUG_OVERLAY_AUTO_HIDE_MS, NULL);
    lv_timer_pause(s_auto_hide);

    bsp_display_unlock();
}

void debug_overlay_set_last_error(const char *msg)
{
    if (!msg) {
        strlcpy(s_last_error, "none", sizeof(s_last_error));
    } else {
        strlcpy(s_last_error, msg, sizeof(s_last_error));
    }
}

const char *debug_overlay_get_last_error(void)
{
    return s_last_error;
}
