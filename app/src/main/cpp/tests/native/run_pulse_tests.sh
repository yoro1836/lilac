#!/bin/sh
# PulseAudio server tests against the real libpulse client and ALSA's
# pulse plugin. Needs g++, libpulse-dev, libasound2-dev and
# libasound2-plugins.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
cpp="$here/../.."
out="${VINE_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$out"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wshadow -fsanitize=address,undefined \
    "$here/test_pulse.cpp" "$cpp/audio/pulse/pa_server.cpp" "$cpp/audio/pulse/pa_wire.cpp" \
    $(pkg-config --cflags --libs libpulse-simple alsa) -lpthread -o "$out/test_pulse"
"$out/test_pulse"
