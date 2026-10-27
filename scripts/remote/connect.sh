#!/usr/bin/env bash
# connect.sh [TOKEN]: adb to a phone that runs `tailcat serve 5555`, through
# localhost:5555. TOKEN defaults to $WAYONDROID_PHONE_TOKEN. A phone on our
# own relay (region 900) needs $WAYONDROID_DERPMAP_URL, the relay's DERP map
# as seen from here; see docs/remote-device.md.
set -euo pipefail
. "$(dirname "$0")/env.sh"
TOKEN=${1:-${WAYONDROID_PHONE_TOKEN:?give the phone\'s tailcat token}}
"$ADB" disconnect 127.0.0.1:5555 >/dev/null 2>&1 || true
pkill -f "^$TAILCAT forward" 2>/dev/null || true
if "$TAILCAT" parse "$TOKEN" 2>/dev/null | grep -q '"RegionID": 900'; then
    export TAILCAT_DERPMAP_URL=${WAYONDROID_DERPMAP_URL:?the phone uses our relay: set WAYONDROID_DERPMAP_URL}
fi
TS_DEBUG_DERP_WS_CLIENT=1 nohup "$TAILCAT" forward "$TOKEN" 5555 > "$TOOLS/forward.log" 2>&1 &
for _ in $(seq 1 30); do
    "$ADB" connect 127.0.0.1:5555 2>/dev/null | grep -q connected && break
    sleep 1
done
"$ADB" -s 127.0.0.1:5555 shell getprop ro.product.model
