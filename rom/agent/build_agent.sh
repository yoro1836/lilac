#!/bin/sh
# Builds vine-agent.jar for /system/framework, needs a JDK and an Android SDK with build-tools
set -e
here="$(cd "$(dirname "$0")" && pwd)"
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[ -n "$sdk" ] || { echo "Set ANDROID_HOME to your Android SDK folder" >&2; exit 1; }
platform="$(ls -d "$sdk"/platforms/android-* 2>/dev/null | sort -V | tail -n 1)"
tools="$(ls -d "$sdk"/build-tools/* 2>/dev/null | sort -V | tail -n 1)"
[ -f "$platform/android.jar" ] || { echo "No platform with android.jar under $sdk/platforms" >&2; exit 1; }
[ -x "$tools/d8" ] || { echo "No d8 under $sdk/build-tools" >&2; exit 1; }

out="${1:-$here/build}"
mkdir -p "$out/classes"
javac --release 8 -Xlint:all -cp "$platform/android.jar" -d "$out/classes" "$here"/src/com/hexadecinull/vineos/agent/*.java
"$tools/d8" --release --min-api 21 --lib "$platform/android.jar" --output "$out" $(find "$out/classes" -name '*.class')
(cd "$out" && zip -q -FS vine-agent.jar classes.dex)
echo "Built $out/vine-agent.jar"
