#!/bin/sh
# Display reader and guest writer tests, no root needed so they can run in CI. Needs g++.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
cpp="$here/../.."
out="${VINE_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$out"

g++ -std=c++17 -O2 -g -Wall -Wextra -I"$cpp/tests/stubs" -I"$cpp/utils" \
    "$here/test_display.cpp" "$cpp/display/pixel_convert.cpp" "$cpp/display/vinefb_reader.cpp" -lpthread -o "$out/test_display"

g++ -std=c++17 -O2 -g -Wall -Wextra -I"$cpp/tests/stubs" -I"$cpp/utils" -I"$cpp/../../../../rom/hwc" \
    "$here/test_vinefb_writer.cpp" "$cpp/display/pixel_convert.cpp" "$cpp/display/vinefb_reader.cpp" -lpthread -o "$out/test_vinefb_writer"

"$out/test_display"
"$out/test_vinefb_writer"
