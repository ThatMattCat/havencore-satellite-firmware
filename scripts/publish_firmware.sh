#!/usr/bin/env bash
# Auto-publish the freshly built firmware to the HavenCore web server so
# the device's "Update Firmware" pull path can grab it. Runs after every
# `idf.py build` via the CMake hook in main/CMakeLists.txt; also callable
# manually via `make publish`.
#
# Configure via .publish.env (gitignored) at the repo root:
#   HC_PUBLISH_DEST=user@havencore.local:/var/www/havencore/firmware/satellite.bin
#   HC_PUBLISH_KEY=~/.ssh/havencore_id_ed25519     # optional
#
# Unconfigured → no-op (exit 0). Failures are non-fatal: the build still
# succeeds; we just print a warning. The device's Push-OTA path doesn't
# need this, only Settings → Update Firmware does.

set -u

BIN="${1:-build/havencore_satellite.bin}"
ENV_FILE="$(dirname "$0")/../.publish.env"

if [ -f "$ENV_FILE" ]; then
    # shellcheck disable=SC1090
    set -a; . "$ENV_FILE"; set +a
fi

if [ -z "${HC_PUBLISH_DEST:-}" ]; then
    exit 0
fi

if [ ! -f "$BIN" ]; then
    echo "publish_firmware: $BIN not found, skipping" >&2
    exit 0
fi

# rsync over ssh: --chmod=F644 makes nginx-readable regardless of source
# perms (mktemp's 0600 default would otherwise 403 the sidecar), and
# rsync writes to a tempfile and atomically renames — so a satellite
# pulling mid-publish gets either the old file or the new one, never a
# half-written one.
RSYNC_SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5"
if [ -n "${HC_PUBLISH_KEY:-}" ]; then
    RSYNC_SSH="$RSYNC_SSH -i ${HC_PUBLISH_KEY}"
fi

# Build the version sidecar JSON. Extract the same `version` string IDF
# bakes into esp_app_desc_t so the device can do an exact strcmp on the
# pull path. IDF derives PROJECT_VER from `git describe --always --tags
# --dirty` when not overridden, so we replicate that here.
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=$(git -C "$REPO_ROOT" describe --always --tags --dirty 2>/dev/null || echo "unknown")
SIZE=$(stat -c%s "$BIN" 2>/dev/null || echo 0)
SHA=$(sha256sum "$BIN" 2>/dev/null | awk '{print $1}')
SHA=${SHA:-unknown}

JSON_TMP=$(mktemp -t satellite.json.XXXXXX)
trap 'rm -f "$JSON_TMP"' EXIT
printf '{"version":"%s","size":%s,"sha256":"%s"}\n' "$VERSION" "$SIZE" "$SHA" > "$JSON_TMP"

# Derive the JSON destination by swapping the trailing .bin filename for
# .json on the dest. Works for both "host:/path/satellite.bin" and
# "host:/path/" forms.
JSON_DEST="${HC_PUBLISH_DEST%satellite.bin}satellite.json"
if [ "$JSON_DEST" = "$HC_PUBLISH_DEST" ]; then
    # No "satellite.bin" suffix to strip — fall back to dropping the last
    # path component and appending satellite.json.
    JSON_DEST="${HC_PUBLISH_DEST%/*}/satellite.json"
fi

echo "==> publish_firmware: rsync $BIN ($SIZE bytes, ver=$VERSION) -> $HC_PUBLISH_DEST"

if rsync -a --chmod=F644 -e "$RSYNC_SSH" "$BIN" "$HC_PUBLISH_DEST"; then
    echo "    publish_firmware: bin ok"
else
    rc=$?
    echo "publish_firmware: bin rsync failed (rc=$rc); build still succeeds" >&2
    exit 0
fi

if rsync -a --chmod=F644 -e "$RSYNC_SSH" "$JSON_TMP" "$JSON_DEST"; then
    echo "    publish_firmware: sidecar ok ($JSON_DEST)"
else
    rc=$?
    echo "publish_firmware: sidecar rsync failed (rc=$rc); bin already published" >&2
fi

exit 0
