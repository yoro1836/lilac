#!/usr/bin/env bash
# connect.sh [TOKEN]: adb to a phone that runs `tailcat serve 5555`, through
# localhost:5555. TOKEN defaults to $WAYONDROID_PHONE_TOKEN. With
# $WAYONDROID_DERPMAP_URL set (our relay's DERP map as seen from here) we
# use our relay; a token naming region 900 requires it. See
# docs/remote-device.md.
set -euo pipefail
. "$(dirname "$0")/env.sh"
TOKEN=${1:-${WAYONDROID_PHONE_TOKEN:?give the phone\'s tailcat token}}
"$ADB" disconnect 127.0.0.1:5555 >/dev/null 2>&1 || true
pkill -f "^$TAILCAT forward" 2>/dev/null || true
# A saved key's address names no region (-1), so the map can't come from it
if [ -n "${WAYONDROID_DERPMAP_URL:-}" ]; then
    export TAILCAT_DERPMAP_URL=$WAYONDROID_DERPMAP_URL
elif "$TAILCAT" parse "$TOKEN" 2>/dev/null | grep -q '"RegionID": 900'; then
    echo "the phone uses our relay: set WAYONDROID_DERPMAP_URL" >&2; exit 1
fi
TS_DEBUG_DERP_WS_CLIENT=1 nohup "$TAILCAT" forward "$TOKEN" 5555 > "$TOOLS/forward.log" 2>&1 &
for _ in $(seq 1 30); do
    "$ADB" connect 127.0.0.1:5555 2>/dev/null | grep -q connected && break
    sleep 1
done
"$ADB" shell getprop ro.product.model
