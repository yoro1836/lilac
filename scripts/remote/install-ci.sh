#!/usr/bin/env bash
# install-ci.sh [--no-wait] [--no-launch] [SHA]: install the dev-apk
# workflow's APK of SHA (default HEAD, pushed) on the connected phone. The
# phone downloads it from the `dev` release itself, which beats pushing it
# through the tunnel. Waits for the build unless --no-wait.
set -euo pipefail
. "$(dirname "$0")/env.sh"
REPO=${WAYONDROID_REPO:-yoro1836/wayondroid}
wait=1 launch=1 sha=
for a in "$@"; do
    case $a in --no-wait) wait=0 ;; --no-launch) launch=0 ;; -*) echo "unknown: $a" >&2; exit 2 ;; *) sha=$a ;; esac
done
sha=$(git -C "$ROOT" rev-parse "${sha:-HEAD}")
built() {
    curl -fsS "https://api.github.com/repos/$REPO/releases/tags/dev" |
        python3 -c 'import json,sys; print(json.load(sys.stdin).get("body") or "")' | grep -q "^$sha"
}
if [ $wait = 1 ]; then
    for _ in $(seq 1 ${WAYONDROID_CI_WAIT_MIN:-90}); do built && break; sleep 60; done
fi
built || { echo "dev release isn't $sha yet" >&2; exit 1; }
apk=/data/local/tmp/tawc-dev/wayondroid-debug.apk
"$ADB" shell "mkdir -p ${apk%/*} && curl -fsSL -o $apk https://github.com/$REPO/releases/download/dev/wayondroid-debug.apk && pm install -r $apk; r=\$?; rm -f $apk; exit \$r"
[ $launch = 0 ] || { "$ADB" shell am force-stop "$PKG"; "$ADB" shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null; }
