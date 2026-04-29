#!/usr/bin/env bash
# Write a build-unique version string to version.txt at the project root,
# so every dev rebuild produces a different esp_app_desc_t.version. This
# is what makes Settings -> Update Firmware actually pull a new build:
# the device-side check is strcmp(running, sidecar.version), and without
# this script that compare is stuck on the same `-dirty` suffix every
# rebuild.
#
# Strategy:
#   - Clean tagged commit (no -dirty, no -gNNNN suffix from git describe):
#     use the tag verbatim. e.g. `v0.2`. Production releases stay pristine.
#   - Anything else (dirty tree OR commits past the latest tag): append
#     `+b<unix-timestamp>` so back-to-back builds get unique versions.
#
# Output: writes version.txt to the project root (gitignored).

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${REPO_ROOT}/version.txt"

GIT_VER=$(git -C "$REPO_ROOT" describe --always --tags --dirty 2>/dev/null || echo "unknown")

# "Clean tagged release" means the describe output is exactly a tag — no
# `-dirty` suffix and no `-<n>-g<sha>` walk-back. Match those forms.
if [[ "$GIT_VER" == *-dirty* ]] || [[ "$GIT_VER" =~ -g[0-9a-f]+$ ]] || [[ "$GIT_VER" == "unknown" ]]; then
    VERSION="${GIT_VER}+b$(date +%s)"
else
    VERSION="$GIT_VER"
fi

# Only rewrite if content changed — keeps mtime stable for clean-release
# builds so CMake configure doesn't churn unnecessarily.
if [ -f "$OUT" ] && [ "$(cat "$OUT" 2>/dev/null)" = "$VERSION" ]; then
    exit 0
fi

printf '%s\n' "$VERSION" > "$OUT"
echo "bump_version: $VERSION"
