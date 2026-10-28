#!/usr/bin/env bash
# send.sh LOCAL DEVICE_PATH: copy a file to the phone through sendit.sh (plain
# HTTPS, MB/s) instead of the tunnel (~0.15 MB/s). Not for anything private:
# the upload is public until it expires. Needs curl on the phone (the S22
# has one, the S24 not).
set -euo pipefail
. "$(dirname "$0")/env.sh"
src=$1 dst=$2
url=$(curl -fsS --data-binary @"$src" "https://sendit.sh/$(basename "$src")" | grep -o 'https://sendit.sh/[^ ]*' | head -1)
[ -n "$url" ] || { echo "sendit.sh upload failed" >&2; exit 1; }
"$ADB" shell "mkdir -p '${dst%/*}' && curl -fsSL -o '$dst' '$url'"
