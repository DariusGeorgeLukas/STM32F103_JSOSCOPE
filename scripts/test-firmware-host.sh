#!/usr/bin/env bash
# Tests the shared device library, then builds and runs the board code that has
# no STM32 dependencies with sanitizers. Finally it checks the generated JSON
# and runs the bench script against the simulated device.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
library="$root/lib/jsoscope-device"
if [[ ! -f "$library/src/protocol.cpp" ]]; then
    echo "lib/jsoscope-device is empty; run: git submodule update --init" >&2
    exit 1
fi
"$library/scripts/test-host.sh"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT

"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$root/firmware/src" -I"$library/include" \
    "$root/firmware/tests/host_test.cpp" \
    "$root/firmware/src/scope_config.cpp" \
    "$library/src/protocol.cpp" \
    -o "$output/host_test"

"$output/host_test"
"$output/host_test" --dump | python3 "$root/scripts/check_firmware_json.py"
python3 "$root/scripts/test_bench_stream.py" "$output/host_test"

if [[ -x "$root/ui/JSOScope/target/release/jsoscope-server" ]]; then
    XDG_CONFIG_HOME="$output/config" python3 "$root/scripts/test_server_e2e.py" \
        --server "$root/ui/JSOScope/target/release/jsoscope-server" \
        --host-test "$output/host_test"
else
    echo "skipped the server test: build jsoscope-server first"
fi
