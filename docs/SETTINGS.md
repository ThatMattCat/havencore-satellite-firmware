# Settings

How user-editable and provisioning state is stored, read, written, and
exposed through the Settings screen. Covers adding a new NVS key,
surfacing it in the UI, and pushing it to the HavenCore server.

Related docs:
- [`PROVISIONING.md`](PROVISIONING.md) — seeding NVS on a fresh device
  via `esptool` (first-boot workflow; does not touch this screen).
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — where settings sit in the boot
  sequence and component graph.

## Storage

All settings live in NVS partition **`nvs`**, namespace **`configuration`**
(constants `uf2_nvs_partition` / `uf2_nvs_namespace` in
`main/settings/settings.c`). The partition is 16 KiB at `0x9000`
(`partitions.csv`).

The in-memory mirror is a single `sys_param_t` struct
(`main/settings/settings.h`), read at boot by
`settings_read_parameter_from_nvs()` and accessed everywhere else via
`settings_get_parameter()`.

### Current schema

| Key | Type | Size | Required? | Default | Set by | Read by |
|-----|------|------|-----------|---------|--------|---------|
| `ssid` | str | 32 | yes | — | esptool (`PROVISIONING.md`) | Wi-Fi init |
| `password` | str | 64 | yes | — | esptool | Wi-Fi init |
| `Base_url` | str | 64 | yes | — | esptool | `havencore_client` (via `sys_param->url`) |
| `voice` | str | 32 | no | `af_heart` | esptool | TTS request body |
| `wake_enabled` | u8 | 1 | no | `1` | esptool | `wake_word_set_enabled()` (currently hardcoded — see `CLAUDE.md`) |
| `device_name` | str | 32 | no | `Satellite` | esptool OR Settings screen | `X-Device-Name` header on every HavenCore request |

**Required vs. optional** is a design choice enforced by
`settings_read_parameter_from_nvs()`:

- **Required** keys `goto err` on `nvs_get_str` failure, which calls
  `settings_factory_reset()` — the device reboots into `ota_0` (TinyUF2)
  so the user can re-provision. This path is currently broken, which is
  why `PROVISIONING.md` uses `esptool` directly.
- **Optional** keys fall back to a default inline (no `goto err`). Use
  this whenever the device can still run without the key set.

## Read path

`main/settings/settings.c:34` — `settings_read_parameter_from_nvs()`.
Called once from `app_main()` in `main/main.c` after `nvs_flash_init()`
and before any consumer. The boot log summarizes what was read:

```
settings: stored ssid:...
settings: stored password:...
settings: stored Base URL:...
settings: voice:af_heart wake_enabled:1 device_name:Satellite
```

## Write path

Most settings are write-once at provisioning time via `esptool`.
Anything the user can edit in-app needs a dedicated setter. Today the
only writable key is `device_name`, with
`settings_set_device_name(const char *name)`:

```c
esp_err_t settings_set_device_name(const char *name) {
    nvs_open_from_partition(..., NVS_READWRITE, &handle);
    nvs_set_str(handle, "device_name", name);
    nvs_commit(handle);
    nvs_close(handle);
    strlcpy(g_sys_param.device_name, name, sizeof(...)); // keep in-memory mirror in sync
    return ESP_OK;
}
```

Pattern to follow for any new writable key:

1. Open NVS in `NVS_READWRITE` mode (`settings_read_parameter_from_nvs`
   opens it read-only).
2. `nvs_set_*` → `nvs_commit` → `nvs_close`.
3. Update the `g_sys_param` mirror so subsequent reads see the new value
   without requiring a reboot.
4. Log the new value for debuggability.

If the setting is consumed by the HTTP client (e.g. stamped onto an
outgoing header), also call the client-side setter here so the running
state stays coherent — see *Identity headers* below for the worked
example.

## Settings screen

The Settings screen is one of five LVGL screens generated from
`squareline/chat_gpt.spj`, at `main/ui/screens/ui_ScreenSettings.c`. The
screen root is `ui_ScreenSettings`; reached by tapping the gear icon on
the Listen screen, dismissed via the back arrow.

### Layout

```
┌──────────────────────────────────────────────┐
│  [←]            Settings              [⟲]   │ ← ui_ImageSettingsBack + title + ui_ImageSettingsReset
├──────────────────────────────────────────────┤
│                                              │
│  ╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌  │ ← ui_PanelSettingsSplitBar (gradient rule)
│                                              │
│  Device Name        [ Kitchen        ]       │ ← ui_PanelSettingsDeviceName row
│                                              │
│                                              │
│  (on-screen keyboard overlay when focused)   │ ← ui_KeyboardSettings (hidden by default)
└──────────────────────────────────────────────┘
```

The parent of the rows is `ui_PanelSettings` — a `LV_FLEX_FLOW_COLUMN`
container filling the bottom 79% of the screen. Each row is a
`LV_FLEX_FLOW_ROW` child that pairs a label with a control.

### The row pattern

Every editable setting should be one flex-row child of `ui_PanelSettings`,
with a label on the left and a control on the right. Copy the
`ui_PanelSettingsDeviceName` block (`ui_ScreenSettings.c`) as a
template:

```c
ui_PanelSettingsFoo = lv_obj_create(ui_PanelSettings);
lv_obj_set_flex_flow(ui_PanelSettingsFoo, LV_FLEX_FLOW_ROW);
lv_obj_set_flex_align(ui_PanelSettingsFoo,
                      LV_FLEX_ALIGN_SPACE_BETWEEN,
                      LV_FLEX_ALIGN_CENTER,
                      LV_FLEX_ALIGN_CENTER);
// height ~19% of the column, full width, transparent bg,
// 10px horizontal padding, no border.

ui_LabelSettingsFoo = lv_label_create(ui_PanelSettingsFoo);
lv_label_set_text(ui_LabelSettingsFoo, "Foo");
// use ui_font_PingFangEN16, white text.

// control goes here: lv_textarea / lv_switch / lv_dropdown / etc.
```

Keep labels in the `ui_font_PingFangEN16` face with `#FFFFFF` text on
the purple-gradient background. The control should be ~55% width so the
label + control clear each other on 320 px.

### Text input flow

The Device Name row uses the LVGL `lv_textarea` + `lv_keyboard` duo.
The keyboard is a **single shared widget** created once per screen and
parented to the screen (not to any row), positioned at
`LV_ALIGN_BOTTOM_MID`, hidden by default. Each textarea's event handler
targets that shared keyboard:

```c
void ui_event_TextareaSettingsDeviceName(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {          // tap the field
        lv_keyboard_set_textarea(ui_KeyboardSettings, ta);
        lv_obj_clear_flag(ui_KeyboardSettings, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) { // tap elsewhere
        lv_obj_add_flag(ui_KeyboardSettings, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_READY) {     // tap the checkmark
        const char *text = lv_textarea_get_text(ta);
        settings_set_device_name(text);
        havencore_client_set_device_name(text);
        lv_obj_add_flag(ui_KeyboardSettings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(ta, LV_STATE_FOCUSED);
    }
}
```

The back button also force-hides the keyboard
(`ui_event_ImageSettingsBack` in `main/ui/ui.c`) so it doesn't leak
across screen transitions.

**Share the keyboard across rows.** If you add a second text field,
reuse `ui_KeyboardSettings` — do not create another one. Create
per-row event handlers that each call `lv_keyboard_set_textarea(ui_KeyboardSettings, <that_ta>)`
on focus.

### SquareLine regeneration hazard

`main/ui/screens/ui_ScreenSettings.c`, `main/ui/ui.c`, and
`main/ui/ui.h` contain hand-edits that are **not** reflected in
`squareline/chat_gpt.spj`. Regenerating from SquareLine will wipe them.
If you must regenerate, re-apply in order:

1. In `ui_ScreenSettings.c`: the `ui_PanelSettingsDeviceName` +
   `ui_LabelSettingsDeviceName` + `ui_TextareaSettingsDeviceName` +
   `ui_KeyboardSettings` block; delete any Region widgets the
   regenerator brings back; swap the event-cb registration from
   `ui_DropdownSettingsRegion` to nothing (the textarea registers its
   own handler inline).
2. In `ui.c`: the `ui_event_TextareaSettingsDeviceName` and
   `ui_event_KeyboardSettings` handlers, the `havencore_client.h` /
   `settings.h` includes, and the keyboard-hide lines inside
   `ui_event_ImageSettingsBack`.
3. In `ui.h`: swap the four `ui_*Region*` externs for the six
   `ui_*DeviceName*` / `ui_*Keyboard*` externs.

Long-term fix: open `chat_gpt.spj` in SquareLine, delete Region, add
Device Name + Keyboard, and commit the updated `.spj` so regeneration
is lossless again.

## Adding a new setting end-to-end

Worked recipe — adapt to your specifics.

### Read-only setting (provisioning time only)

1. `settings.h`: add `#define FOO_SIZE N` and a field to `sys_param_t`.
2. `settings.c`, in `settings_read_parameter_from_nvs()`: add a read
   block. Pick **required** (goto err on failure) or **optional**
   (default inline) per the table convention above.
3. `PROVISIONING.md`: add the key to the example CSV and to the
   "when to re-provision" list.
4. `CLAUDE.md`: add the key to the NVS keys list in Provisioning.
5. This doc: add a row to the schema table.

### User-editable setting

All of the above, plus:

6. `settings.h`: declare `esp_err_t settings_set_foo(...);`.
7. `settings.c`: implement the setter (see the *Write path* section).
8. `ui_ScreenSettings.c`: add a row inside `ui_PanelSettings` with the
   label + control. If the control needs text input, reuse
   `ui_KeyboardSettings`.
9. `ui.h` + top of `ui.c`: add externs for the new widgets, declare
   + implement the event handler.
10. `main/main.c`: if the setting is consumed at init time (e.g. pushed
    to another subsystem), propagate it after the NVS read.

### Setting that flows to the HavenCore server

All of the above, plus:

11. Add the consumer-side state (a static buffer, a header value, a
    request-body field) in the component that uses it — typically
    `components/havencore_client/havencore_client.c`.
12. Expose a setter on that component so the Settings handler can
    update it live without a reboot.
13. Call the consumer setter (a) once at boot in `main.c` after the
    NVS read, and (b) from the Settings event handler right after
    `settings_set_foo()`.

## Identity headers (worked example)

`X-Session-Id` and `X-Device-Name` are stamped onto every HavenCore
request by `components/havencore_client/havencore_client.c`. They
demonstrate the full "NVS → client → wire" pipeline.

- **`X-Session-Id`** — stable per-device, derived from the Wi-Fi STA
  MAC at boot: `havencore_client_init_session_id()` formats
  `selene-<last-4-hex>` into a file-static buffer. No NVS key; survives
  factory reset because the MAC does.
- **`X-Device-Name`** — mirrors `sys_param_t.device_name` into a
  separate file-static buffer via
  `havencore_client_set_device_name()`. Called (a) once at boot in
  `main.c` after `settings_read_parameter_from_nvs()`, and (b) from
  `ui_event_TextareaSettingsDeviceName` when the user taps the
  keyboard checkmark.

Both headers are emitted from a single helper `set_identity_headers()`
invoked at the top of each HTTP function, right after
`esp_http_client_init()`. Empty values are skipped (no header sent).

## Gotchas

- **`NVS_READONLY` vs `NVS_READWRITE`.** The boot-time open is
  read-only; a setter must re-open in read-write mode. Forgetting this
  fails silently at `nvs_set_str` with `ESP_ERR_NVS_READ_ONLY`.
- **Must update the in-memory mirror.** `g_sys_param` is the source of
  truth for every consumer after boot. Writing NVS without updating
  the mirror means the change takes effect only after reboot.
- **Required-key failure = UF2 reboot loop.** Don't add a new key to
  the `goto err` path unless the device genuinely can't run without
  it. A typo in `nvs_config.csv` will brick the device until you
  re-flash.
- **Header size is not string length.** `lv_textarea_set_max_length()`
  takes characters, not bytes; make sure it matches
  `sizeof(foo) - 1` (the `-1` for the trailing NUL).
- **Keyboard leaks across screens.** LVGL does not auto-hide widgets
  on screen transitions. Any screen that contains `ui_KeyboardSettings`
  must explicitly hide it in its "leave" handler.
