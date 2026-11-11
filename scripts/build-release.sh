#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
make -C "$project_dir/firmware" -j"$(nproc)"

printf 'Firmware: %s\n' "$project_dir/firmware/build/stm32f103_jsoscope.bin"
