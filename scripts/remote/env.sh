# Sourced by the scripts here: where the tools live and which app to drive
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TOOLS=${WAYONDROID_TOOLS:-$ROOT/build/remote-tools}
export ANDROID_HOME=${ANDROID_HOME:-$TOOLS/sdk}
ADB=$ANDROID_HOME/platform-tools/adb
TAILCAT=$TOOLS/tailcat
PKG=${WAYONDROID_PACKAGE:-com.hexadecinull.vineos.debug}
HERE=$ROOT/scripts/remote
