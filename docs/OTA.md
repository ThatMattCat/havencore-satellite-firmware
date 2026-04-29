# OTA & Flash Layout

How firmware updates work on a deployed satellite, and how the partition
table is shaped to make A/B updates safe alongside the TinyUF2 recovery
path. OTA infrastructure landed 2026-04-29; partition layout was
re-balanced in the same change.

Related docs:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — boot sequence and how
  `boot_health_task` ties into rollback marking.
- [`PROVISIONING.md`](PROVISIONING.md) — how to migrate an existing
  device onto the OTA partition layout the first time.

## At a glance

Two paths, both writing to whichever slot
`esp_ota_get_next_update_partition()` returns (alternating `ota_0` ↔
`ota_1` — `factory` is OTA-immune):

| Path  | Trigger                              | Endpoint                                   | Audience |
| ----- | ------------------------------------ | ------------------------------------------ | -------- |
| Push  | `make ota IP=<addr>` (build host)    | `POST http://<device>/dev/ota`             | Dev loop |
| Pull  | Settings → Update Firmware           | `GET ${Base_url}/firmware/satellite.bin`   | End user |

Both check `havencore_ota_state_iface_t::is_idle()` first (the iface is
registered in `main.c` and bridges to `sat_state_get() == SAT_STATE_IDLE`);
push returns HTTP 409 mid-conversation, pull aborts before allocating
buffers.

`components/havencore_ota/` owns both paths. The push server
(`havencore_ota_dev_server_start()`) is started once from
`boot_health_task` after the first probe succeeds; it stays up for the
lifetime of the app. `GET /dev/version` returns the running app desc as
JSON — used by `make version IP=<addr>` and as a debug aid.

## Pull path & version-skip

In `app_ui_events.c::EventButtonSettingsUpdate` the device builds two
URLs from the configured `Base_url`:

- `<base>/firmware/satellite.bin`
- `<base>/firmware/satellite.json`

The sidecar JSON is fetched first. If its `version` field equals
`esp_app_get_description()->version`, the device shows "Up to date" on
the GET panel for 2.5 s and returns to IDLE without entering UPDATING.
A missing/unreachable sidecar logs a warning and falls through to an
unconditional pull (so servers without a sidecar still work).
`havencore_ota_pull()` wraps `esp_https_ota` and reboots on success.

## Auto-publish to the web server

`scripts/publish_firmware.sh` runs as a CMake post-build step in
`main/CMakeLists.txt` (custom command depending on the .bin output,
gated by a stamp file so it only re-runs when the bin changes). The
script:

1. Sources `.publish.env` at the repo root (gitignored). If
   `HC_PUBLISH_DEST` is unset, exits 0 silently.
2. Generates `satellite.json` from `git describe --always --tags --dirty`
   (matching the version IDF compiles into `esp_app_desc_t`), the bin's
   byte size, and its sha256.
3. Rsyncs both files to the destination with `--chmod=F644`.

Two reasons for rsync over scp: nginx runs as a non-owner uid in its
container and would 403 the default mktemp 0600 sidecar, and rsync
writes to a tempfile and atomic-renames so a satellite pulling
mid-publish gets either the old file or the new one, never a
half-written one.

Failures are non-fatal — the build still succeeds; the script prints a
warning. A manual `make publish` runs the same script against the
current `build/` artifacts.

## Rollback

After a fresh OTA, the freshly-booted image starts in
`ESP_OTA_IMG_PENDING_VERIFY`. `boot_health_task` calls
`esp_ota_mark_app_valid_cancel_rollback()` after any of the three boot
probes (`/api/status`, `/api/stt/health`, `/api/tts/health`) returns
200. A hard-crash before that point causes the bootloader to roll back
to the previous slot automatically
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).

## Flash layout

See `partitions.csv`. Layout on 16 MB flash (post-OTA rebalance,
2026-04-29):

| Name       | Offset    | Size    | Use                                                                |
| ---------- | --------- | ------- | ------------------------------------------------------------------ |
| `nvs`      | 0x9000    | 16 KB   | Wi-Fi credentials + agent config                                   |
| `otadata`  | 0xd000    | 8 KB    | OTA boot selection (blank → bootloader picks factory)              |
| `phy_init` | 0xf000    | 4 KB    | RF calibration                                                     |
| `factory`  | 0x10000   | 1.5 MB  | TinyUF2 recovery app (factory subtype — never targeted by OTA)     |
| `ota_0`    | 0x190000  | 4 MB    | main app slot A (boot target after first turn-around)              |
| `ota_1`    | 0x590000  | 4 MB    | main app slot B (alternates via `esp_ota_get_next_update_partition`) |
| `storage`  | 0x990000  | 1 MB    | SPIFFS — audio prompts in `spiffs/`                                |
| `model`    | 0xa90000  | 2 MB    | SPIFFS — microWakeWord + future VAD `.tflite` / `.json`            |

`factory` is the only OTA-immune partition subtype reachable via
`esp_ota_set_boot_partition()` — `test` would also be OTA-immune but is
GPIO-hold-only per the bootloader, and `ota_X` would be overwritten by
the normal OTA cycle. So TinyUF2 has to live in `factory`.

### Boot flow with empty otadata

On first boot, `otadata` is all 0xff (initial state); the bootloader's
invalid-otadata path falls back to `factory` (TinyUF2). TinyUF2's
`app_main` immediately calls `esp_ota_set_boot_partition(ota_0)` to
schedule the next boot into the main app, then continues into the USB
mass-storage UI loop. After the user edits `CONFIG.INI` and reboots,
the bootloader follows otadata → ota_0 (main app). OTA writes alternate
ota_0 ↔ ota_1; `esp_ota_get_next_update_partition()` walks the OTA
chain only, so factory (TinyUF2) is never overwritten.

### Software-triggered factory reset

`settings_factory_reset()` calls
`esp_ota_set_boot_partition(factory_partition)`, which erases
`otadata`. Next boot the bootloader sees invalid otadata and falls back
to factory (TinyUF2) again. See [Gotchas](#gotchas) for why erasing
otadata is the right primitive.

### `idf.py flash` quirk

parttool.py's `--partition-boot-default` returns the factory offset
whenever a factory partition exists, so IDF auto-flashes the project
bin to factory by default. We override in `main/CMakeLists.txt` with
two `esptool_py_flash_to_partition()` calls — one writes TinyUF2 to
factory (last-write-wins on offset 0x10000 in `flasher_args.json`'s
deduplicated map), the other writes the main app to `ota_0` explicitly.

Side-effect: `idf.py build` prints
`Warning: 1/3 app partitions are too small for binary havencore_satellite.bin`
for factory — expected, harmless.

## Gotchas

Hard-won from the partition pass; re-read before another one.

- **`app, test` subtype isn't software-bootable.** First pass put
  TinyUF2 in a `test` partition and called
  `esp_ota_set_boot_partition()` on it, expecting otadata to point
  there. `esp_ota_set_boot_partition()` for non-factory subtypes calls
  `esp_rewrite_ota_data(subtype)`, and `SUB_TYPE_ID(0x20)` is `0` —
  same as ota_0 — so the test partition's seq number actually maps to
  `ota_0`. The bootloader walks the OTA chain
  (`bootloader_utility_load_boot_image` from `start_index` backward to
  `FACTORY_INDEX`), and `TEST_APP_INDEX` is only entered when
  `start_index == TEST_APP_INDEX`, which is set exclusively by the
  GPIO-hold path in `bootloader_start.c`. So `test` partitions are
  GPIO-only.
- **`esp_ota_set_boot_partition(factory)` works by erasing otadata.**
  Looking at `esp_ota_ops.c`: for the factory subtype, the function
  finds the otadata partition and calls `esp_partition_erase_range()`
  on it. Next boot, the bootloader sees all-0xff otadata, and with a
  factory partition present, falls back to factory.
- **IDF auto-flashes the project bin to whichever partition `parttool.py
  --partition-boot-default` returns.** That prefers factory. We work
  around by adding a custom
  `esptool_py_flash_to_partition(flash "factory" "${nvs_dst_file}")`
  (last-write-wins on the duplicated offset in `flasher_args.json`)
  and an explicit `esptool_py_flash_to_partition(flash "ota_0" ...)`
  for the main app. The build emits `Warning: 1/3 app partitions are
  too small for binary havencore_satellite.bin` for factory — expected.

## Watch-items

- **Binary headroom: ~540 KB per slot** (3.46 MB binary, 4 MB slot,
  ~13% free). VAD code adds little (TFLite runtime is shared with
  microWakeWord), but landing AEC + WebSocket on top of VAD could push
  the binary over. If `idf.py size` shows < 200 KB free in
  `havencore_satellite.bin`, repartition: shrink `model` to 1 MB and
  grow each app slot to ~4.5 MB.
- **Version-skip uses `git describe --always --tags --dirty`** on both
  ends (IDF compiles it into `esp_app_desc_t::version`; the publish
  script re-derives it). If you build twice from the same SHA with
  different uncommitted edits, both get tagged `<sha>-dirty` and the
  second build will be falsely skipped on the device. Commit the
  change to roll the hash forward when version-skip matters.
- **No firmware signing yet.** Intentional per the trusted-LAN scope.
  Add once a satellite leaves the LAN.
