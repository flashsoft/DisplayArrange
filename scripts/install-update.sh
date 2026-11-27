#!/bin/bash
# DisplayArrange OTA update installer
# Called with sudo, expects the new .app path as argument
set -e

NEW_APP="$1"
TARGET="/Applications/DisplayArrange.app"
BACKUP="/Applications/DisplayArrange.app.backup"

if [ -z "$NEW_APP" ] || [ ! -d "$NEW_APP" ]; then
    echo "usage: $0 <path-to-new.app>" >&2
    exit 1
fi

rm -rf "$BACKUP"
mv "$TARGET" "$BACKUP" 2>/dev/null || true
cp -R "$NEW_APP" "$TARGET"
rm -rf "$BACKUP"
echo "installed to $TARGET"
