#!/bin/sh
# Builds the fake ROM and the engine test runner on the host and runs it. Needs root, g++, python3, mke2fs, zlib.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
cpp="$here/../.."
out="${VINE_TEST_OUT:-/tmp/vine-native-tests}"
mkdir -p "$out"

g++ -std=c++17 -O2 -Wall -Wextra -static "$here/fake_init.cpp" -o "$out/fake_init"
g++ -std=c++17 -O2 -Wall -Wextra -static "$here/fake_tool.cpp" -o "$out/fake_tool"
python3 "$here/make_fixture.py" "$out/fixture" "$out/fake_init" "$out/fake_tool" > /dev/null

g++ -std=c++17 -O1 -g -Wall -Wextra -I"$cpp/tests/stubs" -I"$cpp/utils" \
    "$here/test_container.cpp" \
    "$cpp"/container/*.cpp "$cpp/utils/vine_utils.cpp" \
    -lz -o "$out/test_container"

g++ -std=c++17 -O1 -g -Wall -Wextra -I"$cpp/tests/stubs" -I"$cpp/utils" \
    "$cpp"/helper/*.cpp "$cpp"/container/*.cpp "$cpp"/diagnostics/host_probe.cpp "$cpp"/diagnostics/namespace_probe.cpp \
    "$cpp/utils/vine_utils.cpp" -lz -o "$out/vine_helper"

g++ -std=c++17 -O2 -g -Wall -Wextra -I"$cpp/tests/stubs" -I"$cpp/utils" \
    "$here/test_display.cpp" "$cpp/display/pixel_convert.cpp" "$cpp/display/vinefb_reader.cpp" -lpthread -o "$out/test_display"

g++ -std=c++17 -O2 -g -Wall -Wextra -I"$cpp/tests/stubs" -I"$cpp/utils" -I"$cpp/../../../../rom/hwc" \
    "$here/test_vinefb_writer.cpp" "$cpp/display/pixel_convert.cpp" "$cpp/display/vinefb_reader.cpp" -lpthread -o "$out/test_vinefb_writer"

"$out/test_display"
"$out/test_vinefb_writer"
"$out/test_container" "$out/fixture"
python3 "$here/test_helper.py" "$out/fixture" "$out/vine_helper"
