#!/bin/bash
set -e

# ESP-IDF v4.4.7 environment
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v4.4.7}"
# Path to your esp-matter v1.0 checkout containing examples/itho_fan
export ESP_MATTER_PATH="${ESP_MATTER_PATH:-$HOME/Documents/git/esp-matter-v1.0-clone}"

# Add ESP-IDF Python venv to PATH first (has python, pip, idf.py, cmake, ninja, etc.)
export PATH="$HOME/.espressif/python_env/idf4.4_py3.12_env/bin:$PATH"

# Add gn to PATH (from Pigweed bootstrap CIPD) — do NOT add pigweed-venv/bin
CHIP_ROOT="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip"
export PATH="$CHIP_ROOT/.environment/cipd/packages/pigweed:$PATH"

# ESP-IDF tools (this sets up the correct Python venv and toolchain)
export IDF_TOOLS_PATH="$HOME/.espressif"
. $IDF_PATH/export.sh > /dev/null 2>&1

# Set esp-matter path for CMake
export ESP_MATTER_PATH

# Build
cd "$ESP_MATTER_PATH/examples/itho_fan"
idf.py build
