/*
 * SPDX-FileCopyrightText: 2025 HavenCore
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "state.h"
#include "app_ui_ctrl.h"
#include "esp_log.h"

static const char *TAG = "state";
static sat_state_t g_state = SAT_STATE_IDLE;

void sat_state_init(void)
{
    g_state = SAT_STATE_IDLE;
    ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
}

void sat_state_set(sat_state_t s)
{
    if (s == g_state) {
        return;
    }
    ESP_LOGI(TAG, "%s -> %s", sat_state_name(g_state), sat_state_name(s));
    g_state = s;

    switch (s) {
    case SAT_STATE_IDLE:
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
        break;
    case SAT_STATE_LISTENING:
        ui_ctrl_show_panel(UI_CTRL_PANEL_LISTEN, 0);
        break;
    case SAT_STATE_UPLOADING:
    case SAT_STATE_THINKING:
        ui_ctrl_show_panel(UI_CTRL_PANEL_GET, 0);
        break;
    case SAT_STATE_SPEAKING:
        ui_ctrl_show_panel(UI_CTRL_PANEL_REPLY, 0);
        break;
    case SAT_STATE_ERROR:
        ui_ctrl_show_panel(UI_CTRL_PANEL_ERROR, 0);
        break;
    }
}

sat_state_t sat_state_get(void)
{
    return g_state;
}

const char *sat_state_name(sat_state_t s)
{
    switch (s) {
    case SAT_STATE_IDLE:       return "IDLE";
    case SAT_STATE_LISTENING:  return "LISTENING";
    case SAT_STATE_UPLOADING:  return "UPLOADING";
    case SAT_STATE_THINKING:   return "THINKING";
    case SAT_STATE_SPEAKING:   return "SPEAKING";
    case SAT_STATE_ERROR:      return "ERROR";
    }
    return "?";
}
