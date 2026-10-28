#!/usr/bin/env bash
# deploy.sh [--no-build] [--no-launch] [--sendit]: build the debug app,
# install it on the connected phone and start it. --sendit sends the APK
# through sendit.sh (send.sh) instead of the tunnel
set -euo pipefail
. "$(dirname "$0")/env.sh"
build=1 launch=1 sendit=0
for a in "$@"; do
    case $a in --no-build) build=0 ;; --no-launch) launch=0 ;; --sendit) sendit=1 ;; *) echo "unknown: $a" >&2; exit 2 ;; esac
done
cd "$ROOT"
# Maven Central fails at random through cloud egress proxies; the init
# script puts Google's mirror of it first
[ $build = 0 ] || ./gradlew --init-script "$HERE/maven-mirror.init.gradle.kts" :app:assembleDebug --console=plain -q
apk=app/build/outputs/apk/debug/app-debug.apk
if [ $sendit = 0 ]; then
    "$ADB" install -r $apk
else
    t=/data/local/tmp/tawc-dev/app-debug.apk
    "$HERE/send.sh" $apk $t
    "$ADB" shell "pm install -r $t; r=\$?; rm -f $t; exit \$r"
fi
[ $launch = 0 ] || { "$ADB" shell am force-stop "$PKG"; "$ADB" shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null; }
