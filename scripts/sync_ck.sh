#!/bin/bash
# sync_ck.sh - Sync Concurrency Kit (ck) into depend/ck
#
# Usage:
#   # Use an existing local ck checkout
#   CK_SRC=../ck ./scripts/sync_ck.sh
#   # Or let the script clone a fresh copy into /tmp/ck-sync
#
# This script:
#   1. Obtains Concurrency Kit sources (CK_SRC or on-demand clone).
#   2. Replaces depend/ck with the fresh contents (excluding .git).
#   3. Prints a brief status summary.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DEST="$PROJECT_ROOT/depend/ck"

CK_REPO_URL="https://github.com/concurrencykit/ck.git"
CK_SRC="${CK_SRC:-}"

cleanup_clone() {
    if [[ -n "$TEMP_CLONE" && -d "$TEMP_CLONE" ]]; then
        rm -rf "$TEMP_CLONE"
    }
}
trap cleanup_clone EXIT

echo "=== Concurrency Kit sync ==="

# 1. Prepare source
if [[ -n "$CK_SRC" ]]; then
    if [[ ! -d "$CK_SRC" ]]; then
        echo "Error: CK_SRC does not exist: $CK_SRC"
        exit 1
    fi
    echo "Using CK_SRC: $CK_SRC"
else
    TEMP_CLONE="$(mktemp -d /tmp/ck-sync-XXXXXX)"
    echo "Cloning ck from $CK_REPO_URL -> $TEMP_CLONE"
    git clone --depth 1 "$CK_REPO_URL" "$TEMP_CLONE" >/dev/null
    CK_SRC="$TEMP_CLONE"
fi

# 2. Replace destination
echo "Replacing $DEST"
rm -rf "$DEST"
mkdir -p "$DEST"

rsync -a --delete \
    --exclude ".git" \
    "$CK_SRC"/ "$DEST"/

# 3. Summary
FILES_COUNT=$(find "$DEST" -type f | wc -l)
echo "✅ ck synced to $DEST ($FILES_COUNT files)"

echo "Next steps: rebuild as needed (e.g., cargo build or CMake)."


