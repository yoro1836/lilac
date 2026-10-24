#!/bin/sh
# Wayland server tests against the real libwayland-client. Needs g++,
# libwayland-dev, wayland-protocols and wayland-scanner.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
cpp="$here/../.."
out="${VINE_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$out"
xml="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$xml" "$out/xdg-shell-client-protocol.h"
wayland-scanner private-code "$xml" "$out/xdg-shell-protocol.c"
gcc -c "$out/xdg-shell-protocol.c" -o "$out/xdg-shell-protocol.o"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wshadow -fsanitize=address,undefined -I"$out" -I"$cpp/../../../../rom/hwc" \
    "$here/test_wayland.cpp" "$cpp/display/wayland/wl_server.cpp" "$cpp/display/wayland/vinefb_sink.cpp" \
    "$out/xdg-shell-protocol.o" -lwayland-client -lpthread -o "$out/test_wayland"
"$out/test_wayland"
