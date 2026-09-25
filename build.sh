#!/bin/sh
# Build itho-matter-fan: esp-matter release/v1.5 + ESP-IDF v5.5.5 + arduino-esp32 3.3.12
#
# Prerequisites:
#   - ESP-IDF v5.5.5 checked out at $IDF_PATH (default ~/esp/esp-idf-v5.5.5)
#     with esp32 tools installed (./install.sh esp32)
#   - esp-matter release/v1.5 at $ESP_MATTER_PATH (default ~/git/esp-matter-v1.5-clone)
#     with connectedhomeip submodule bootstrapped (scripts/bootstrap.sh)
#   - main/local_config.h copied from main/local_config.example.h with your remote ID
set -e

HERE=$(cd "$(dirname "$0")" && pwd)

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v5.5.5}"
export IDF_TOOLS_PATH="$HOME/.espressif"
export ESP_MATTER_PATH="${ESP_MATTER_PATH:-$HOME/git/esp-matter-v1.5-clone}"
export _PW_ACTUAL_ENVIRONMENT_ROOT="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/.environment"

# Arduino core as an IDF component (pinned tag). Two local patches gate the
# --wrap link options on Arduino's own Matter library, which we disable because
# we use esp-matter from source directly (see patches/).
ARDUINO_DIR="$HERE/components/arduino"
if [ ! -d "$ARDUINO_DIR" ]; then
    echo "Cloning arduino-esp32 3.3.12 into components/arduino ..."
    git clone --depth 1 --branch 3.3.12 https://github.com/espressif/arduino-esp32.git "$ARDUINO_DIR"
fi
if ! grep -q "NOTE (itho_fan)" "$ARDUINO_DIR/CMakeLists.txt"; then
    echo "Applying arduino wrap-gating patch ..."
    git -C "$ARDUINO_DIR" apply "$HERE/patches/arduino-3.3.12-wrap-gating.patch"
fi

# IDF 5.5 python venv and chip bootstrap env (gn/ninja) first, so nothing
# falls back to an ancient system python.
export PATH="$HOME/.espressif/python_env/idf5.5_py3.12_env/bin:$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/.environment/cipd/packages/pigweed:$IDF_PATH/tools:$PATH"

# Toolchain + tool paths (idf_tools.py export), without export.sh's abort-on-missing quirk
eval "$(python3 "$IDF_PATH/tools/idf_tools.py" export)"

cd "$HERE"
exec idf.py "$@"
