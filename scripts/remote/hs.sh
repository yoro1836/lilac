#!/usr/bin/env bash
# hs.sh CMD ARGS...: one request to the debug app's rootless helper (start,
# stop, status, exec ...), through its debug socket
set -euo pipefail
. "$(dirname "$0")/env.sh"
"$ADB" shell "[ -f /data/local/tmp/vine-dev/hs.sh ]" 2>/dev/null ||
    { "$ADB" shell mkdir -p /data/local/tmp/vine-dev; "$ADB" push "$HERE/device-hs.sh" /data/local/tmp/vine-dev/hs.sh >/dev/null; }
"$ADB" shell "run-as $PKG sh /data/local/tmp/vine-dev/hs.sh $(printf '%q ' "$@")"
