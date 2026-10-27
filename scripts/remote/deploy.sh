#!/usr/bin/env bash
# deploy.sh [--no-build] [--no-launch]: build the debug app, install it on
# the connected phone and start it
set -euo pipefail
. "$(dirname "$0")/env.sh"
build=1 launch=1
for a in "$@"; do
    case $a in --no-build) build=0 ;; --no-launch) launch=0 ;; *) echo "unknown: $a" >&2; exit 2 ;; esac
done
cd "$ROOT"
# Maven Central fails at random through cloud egress proxies; the init
# script puts Google's mirror of it first
[ $build = 0 ] || ./gradlew --init-script "$HERE/maven-mirror.init.gradle.kts" :app:assembleDebug --console=plain -q
"$ADB" install -r app/build/outputs/apk/debug/app-debug.apk
[ $launch = 0 ] || { "$ADB" shell am force-stop "$PKG"; "$ADB" shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null; }
