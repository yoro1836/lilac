#!/usr/bin/env bash
# setup.sh: the Android SDK and tailcat for driving a phone from a fresh
# (cloud) checkout, under build/remote-tools. Needs Go and a JDK.
set -euo pipefail
. "$(dirname "$0")/env.sh"
mkdir -p "$TOOLS"
"$ROOT/scripts/ci/setup-android-sdk.sh"
# ts_debug_websockets: reach DERP relays over websockets, all an HTTPS-only
# egress proxy lets through (TS_DEBUG_DERP_WS_CLIENT=1 turns it on)
[ -x "$TAILCAT" ] || GOBIN="$TOOLS" go install -tags ts_debug_websockets github.com/tailscale/tailcat/cmd/tailcat@v0.7.0
"$ADB" version | head -1
"$TAILCAT" --version
