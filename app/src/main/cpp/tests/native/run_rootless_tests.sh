#!/bin/sh
# The rootless helper against stand-ins for tawcroot and its servers. No
# root needed. Needs g++, gcc, python3, mke2fs, zlib.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
cpp="$here/../.."
out="${VINE_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$out"
gcc -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$cpp/../../../../tawcroot/include" \
    -c "$cpp/rootless/ext4_extract.c" -o "$out/ext4_extract.o"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$cpp/tests/stubs" -I"$cpp/utils" \
    "$cpp"/helper/*.cpp "$cpp"/container/*.cpp "$cpp"/diagnostics/host_probe.cpp "$cpp"/diagnostics/namespace_probe.cpp \
    "$cpp/utils/vine_utils.cpp" "$cpp"/rootless/rootless_*.cpp "$cpp/display/wayland/input_lines.cpp" "$out/ext4_extract.o" \
    -lz -o "$out/vine_helper"
python3 "$here/test_rootless.py" "$out/vine_helper"
