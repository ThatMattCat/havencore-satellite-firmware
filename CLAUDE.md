# CLAUDE.md

ESP-IDF v5.5 firmware for an ESP32-S3-BOX-3 voice satellite that talks to a self-hosted HavenCore agent on the LAN. MVP is touch-to-talk: tap → STT → chat → TTS → playback.

Most of the detail lives in `docs/` — [`docs/README.md`](docs/README.md) is the index. Topic docs:

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — hardware, component graph, boot, turn-level FSM, HTTP contracts, UI.
- [`docs/AUDIO.md`](docs/AUDIO.md) — wake-word, simple_vad, listen-window endpointing, follow-up window (incl. onset-tuning post-mortem).
- [`docs/OTA.md`](docs/OTA.md) — OTA push/pull, sidecar version-skip, auto-publish, partition layout, gotchas worth re-reading before another partition pass.
- [`docs/PROVISIONING.md`](docs/PROVISIONING.md) — UF2 mass-storage flow + esptool/CSV recovery appendix.
- [`docs/SETTINGS.md`](docs/SETTINGS.md) — NVS schema and the recipe for adding a new user-editable setting (storage + UI row + HTTP plumbing).
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — current status, known issues, deferred work, MVP checklist.

When changing direction, update ROADMAP first. Before adding or restructuring any doc, skim [`docs/README.md`](docs/README.md) § Writing guidelines — it covers structure, where new content goes, and the convention for retiring deferred items.

## Build & flash

ESP-IDF v5.5 is at `~/esp/v5.5/esp-idf`. `$IDF_PATH` is not preset — `source ~/esp/v5.5/esp-idf/export.sh` in each new shell.

```sh
# one-time (builds factory_nvs.bin used for first-boot provisioning)
cd factory_nvs && idf.py build && cd ..

idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.box-3" build
idf.py -p /dev/ttyACM0 flash monitor

# After first flash, the dev loop is OTA over the network:
make build                   # bump version.txt + idf.py build (use this, not bare `idf.py build`)
make ota IP=10.0.0.42        # push build/havencore_satellite.bin
make deploy IP=10.0.0.42     # build + ota in one shot
make version IP=10.0.0.42    # GET /dev/version on the device
make publish                 # rsync bin + sidecar to the HavenCore web server
```

`idf.py build` auto-runs `scripts/publish_firmware.sh` as a CMake post-build step — configure via `.publish.env` (gitignored; see `.publish.env.example`). Unconfigured → silent no-op. See [`docs/OTA.md`](docs/OTA.md) for the version-skip / sidecar flow and why rsync (not scp).

**Use `make build`, not bare `idf.py build`, for dev OTA-pull to work.** `scripts/bump_version.sh` stamps `version.txt` with a fresh unix timestamp on every dev build (clean tagged commits stay pristine — `git tag v0.2` produces a `v0.2` release with no suffix). Without the bump, the device's strcmp on the pull path treats every `-dirty` rebuild as the same version and refuses to flash. Major version bumps remain manual via `git tag`.

The BOX-3 device is attached from a remote Windows host (10.0.0.88) via usbipd/usbip — see `~/remote-usb.txt` for the attach sequence before `/dev/ttyACM0` appears.

## Watch-outs unique to this repo

**`managed_components/` patch.** The directory is gitignored and regenerated on `idf.py fullclean` or dependency re-resolve. After either, run `./patches/apply.sh` — re-applies `patches/esp-box-3-scl_speed_hz.patch` (clears `tp_io_config.scl_speed_hz` so `bsp_touch_new()` doesn't crash on the legacy I²C path). The script is idempotent.

**Hand-edits in SquareLine-generated files.** `main/ui/screens/ui_ScreenSettings.c`, `main/ui/ui.c`, and `main/ui/ui.h` carry manual edits that don't exist in `squareline/chat_gpt.spj`:

- Settings rows for Device Name (textarea + on-screen keyboard), Listen Cap, Silence Timeout, Follow-Up Window (sliders + value labels), and Update Firmware (label + button).
- Removal of the dead Region Select dropdown.

If you regenerate `main/ui/` from SquareLine, re-apply: (1) the `ui_PanelSettings{DeviceName,ListenCap,Silence,FollowUp,Update}` blocks plus the matching textarea / sliders / value labels / button / `ui_KeyboardSettings` in `ui_ScreenSettings.c`; (2) the matching `ui_event_*` handlers in `ui.c` (`TextareaSettingsDeviceName`, `KeyboardSettings`, `SliderSettings{ListenCap,Silence,FollowUp}`, `ButtonSettingsUpdate`); (3) the back-button keyboard-hide lines; (4) the `app_sr.h` include in `ui.c`; (5) externs in `ui.h`; (6) the `EventButtonSettingsUpdate` declaration in `ui_events.h`. Delete any regenerated Region widgets.

**Provisioning.** Pristine devices boot into TinyUF2 mass-storage (factory partition); user edits `CONFIG.INI` on the mounted USB drive. Same flow is reachable from the running app via Settings → factory-reset. See `docs/PROVISIONING.md` for the required NVS keys (`ssid`, `password`, `Base_url`) and the full schema.
