# Itho Matter Fan

Native Matter support for Itho Daalderop HRU 300 WTW ventilation units, eliminating the need for Homebridge or other bridges.

Runs directly on the ESP32 add-on board (ithowifi project by arjenhiemstra) with a CC1101 RF module. Exposes the ventilation unit as a Matter fan device with 3 speed settings (Low, Medium, High — the HRU 300 cannot be turned off), plus four temperature sensors and a Summer Night Boost switch.

## How It Works

```
Matter Controller (Apple Home / Google Home)
        │  Matter over WiFi
        ▼
    ESP32 (ithowifi add-on board)
        │  CC1101 RF 868MHz
        ▼
    Itho HRU 300 ventilation unit
```

The ESP32 acts as a virtual RF remote control via the CC1101 module. Matter FanMode/PercentSetting commands are translated to Itho RF commands (opcode 0x22F1) and sent wirelessly to the HRU 300. Status is polled over I2C with the 0x2401 status query (the per-field format table is learned once via 0x2400); the absolute fanspeed field drives the reported mode so the controller always shows the current fan state. RF reception is interrupt-driven via the CC1101 GDO pin, and the unit's 31DA RF broadcasts are received as a secondary feedback source.

### FanMode Mapping

The HRU 300 cannot be turned off, so Off maps to Low and the percentage slider snaps to three detents (33/66/100):

| Matter FanMode | Value | PercentSetting | Itho RF Command |
|---|---|---|---|
| Off | 0 | 0-33% | IthoLow (4) |
| Low | 1 | 33% | IthoLow (4) |
| Medium | 2 | 66% | IthoMedium (5) |
| High | 3 | 100% | IthoHigh (6) |
| Auto | 5 | — | IthoAuto (11) |

### Sensors and Switches

Four temperature sensors (bridged endpoints, named via NodeLabel): Outside, Supply (inlet), Extract, Exhaust — from the 0x2401 status fields 2/5/6/7. The Summer Night Boost switch reads and writes the corresponding 0x2410 setting (index auto-detected, 95 or 96 depending on the unit's settings version).

### FanInfo Feedback (31DA RF messages)

| FanInfo byte | Meaning | Mapped FanMode | PercentCurrent |
|---|---|---|---|
| 0x00 | Off | 0 | 0% |
| 0x01 | Low | 1 | 33% |
| 0x02 | Medium | 2 | 66% |
| 0x03 | High | 3 | 100% |
| 0x15 | Away | 0 | 0% |
| 0x18 | Auto | 5 | 50% |
| 0x19 | AutoNight | 5 | 50% |

## Hardware Requirements

- **ESP32-WROOM-32** with 4MB flash (ithowifi add-on board, non-CVE variant)
- **CC1101 RF module** (868MHz) connected via SPI
- **Itho HRU 300** ventilation unit

### CC1101 SPI Wiring

| CC1101 Pin | ESP32 GPIO |
|---|---|
| SCK | GPIO 18 |
| MISO (MISO) | GPIO 19 |
| MOSI (SI) | GPIO 23 |
| CS (SS) | GPIO 5 |
| VCC | 3.3V |
| GND | GND |

### Board GPIO Assignments (non-CVE)

| Function | GPIO |
|---|---|
| WiFi LED | 17 |
| Status LED | 16 |
| Fail Safe button | 32 |
| I2C IRQ | 4 |
| CC1101 SCK | 18 |
| CC1101 MISO | 19 |
| CC1101 MOSI | 23 |
| CC1101 CS | 5 |

## Prerequisites

### 1. ESP-IDF v4.4.7

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git esp-idf-v4.4.7
cd esp-idf-v4.4.7
git checkout v4.4.7
git submodule update --init --recursive
./install.sh esp32
```

### 2. esp-matter v1.0

```bash
cd ~/Documents/git
git clone --recursive https://github.com/espressif/esp-matter.git esp-matter-v1.0
cd esp-matter-v1.0
git checkout v1.0
git submodule update --init --recursive

# Checkout submodules for ESP32 target
python connectedhomeip/connectedhomeip/scripts/checkout_submodules.py --shallow --platform esp32
```

### 3. Pigweed Bootstrap (esp-matter dependency)

The Pigweed bootstrap is required for the gn build system used by the Matter SDK. Run it from the esp-matter directory:

```bash
cd connectedhomeip/connectedhomeip
source scripts/bootstrap.sh
```

**Note:** The bootstrap may fail on macOS due to missing `pkg-config` or OpenSSL. This is expected — the critical tools (gn, ninja, CIPD packages) are installed even if the host tools build fails. After bootstrap, verify that `gn` and `ninja` are available.

If `gn` is not on PATH, add it manually:

```bash
export PATH="$PWD/.environment/cipd/packages/pigweed:$PATH"
```

### 4. Arduino-ESP32 v2.0.17 (CC1101 SPI dependency)

The CC1101 RF code uses Arduino's SPI library. Arduino-ESP32 is added as an ESP-IDF component:

```bash
cd <this-repo>
mkdir -p components
git clone --depth 1 --branch 2.0.17 https://github.com/espressif/arduino-esp32.git components/arduino
```

## Build Setup

### Install this example into esp-matter

Copy or symlink this repository into the esp-matter examples directory:

```bash
# Option A: Symlink (recommended, keeps repo in place)
ln -s /path/to/itho-matter-fan /path/to/esp-matter-v1.0/examples/itho_fan

# Option B: Copy
cp -r /path/to/itho-matter-fan /path/to/esp-matter-v1.0/examples/itho_fan
```

### Build

Use the included build script, which sets up the correct environment:

```bash
./build.sh
```

Or set up the environment manually:

```bash
# ESP-IDF environment
export IDF_PATH="$HOME/esp/esp-idf-v4.4.7"
export IDF_TOOLS_PATH="$HOME/.espressif"
. $IDF_PATH/export.sh

# gn from Pigweed CIPD
export ESP_MATTER_PATH="/path/to/esp-matter-v1.0"
CHIP_ROOT="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip"
export PATH="$CHIP_ROOT/.environment/cipd/packages/pigweed:$PATH"

# Build
cd "$ESP_MATTER_PATH/examples/itho_fan"
idf.py build
```

### Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Or use esptool directly (see the output of `idf.py build` for the exact command).

## Binary Size

| Component | Size |
|---|---|
| itho_fan.bin | 1.24 MB |
| App partition | 1.88 MB |
| Free space | 645 KB (34%) |

## Configuration

### Virtual Remote ID

The firmware needs the RF remote ID your HRU 300 listens to. Copy `main/local_config.example.h` to `main/local_config.h` (gitignored) and set your own ID:

```cpp
#define ITHO_REMOTE_ID 0x12, 0x34, 0x56
```

The easiest source is a physical remote you already use: sniff its ID from its RF traffic (the ithowifi firmware's RF log shows it). Using an existing remote's ID means no joining is needed — the HRU 300 already trusts it. You can also pick a fresh ID and join it to the unit (see below).

### Pairing with the HRU 300

If your virtual remote ID is not yet registered with the HRU 300:

1. Put the HRU 300 into join mode (press and hold the button on the ventilation unit)
2. Trigger a join command from the ESP32 (future: via the fail safe button on GPIO 32)
3. The HRU 300 will register the virtual remote ID

If you copied the remote ID from an existing physical remote, it should already be registered and no joining is needed.

### Matter Commissioning

After flashing, commission the device using a Matter controller (Apple Home, Google Home, etc.):

1. Open your Matter controller app
2. Add a new device
3. Scan the Matter QR code (or use the manual pairing code from serial output)
4. The device will join your WiFi network and appear as a fan

## Project Structure

```
itho-matter-fan/
├── README.md                  # This file
├── build.sh                   # Build script with environment setup
├── CMakeLists.txt             # ESP-IDF project CMakeLists
├── partitions.csv             # 4MB flash partition table (dual OTA)
├── sdkconfig.defaults         # ESP-IDF default config (BT, WiFi, FreeRTOS 1000Hz)
├── .gitignore
└── main/
    ├── CMakeLists.txt         # Main component CMakeLists
    ├── app_main.cpp           # Matter FanControl + CC1101 integration
    ├── cc1101/                # CC1101 RF driver (from ithowifi project)
    │   ├── CC1101.cpp         # CC1101 SPI driver
    │   ├── CC1101.h
    │   ├── CC1101Packet.h
    │   ├── IthoCC1101.cpp     # Itho RF protocol layer
    │   ├── IthoCC1101.h
    │   ├── IthoPacket.h       # Itho command definitions
    │   └── sys_log.h          # Minimal logging stub (maps to ESP_LOG)
    └── zap-generated/         # ZAP cluster config (from esp-matter light example)
        ├── af-gen-event.h
        ├── endpoint_config.h
        └── empty_file.cpp
```

## Credits

- **CC1101 RF code**: From the [ithowifi project](https://github.com/arjenhiemstra/ithowifi) by arjenhiemstra (GPLv3)
- **esp-matter**: [Espressif Matter SDK](https://github.com/espressif/esp-matter) v1.0
- **Arduino-ESP32**: [Espressif Arduino core](https://github.com/espressif/arduino-esp32) v2.0.17
- **ESP-IDF**: [Espressif IoT Development Framework](https://github.com/espressif/esp-idf) v4.4.7

## Known Issues

- **Arduino `-DESP32` conflict**: Arduino-ESP32 defines `-DESP32` as a preprocessor macro, which conflicts with the Matter SDK's use of `ESP32` in include paths. Fixed by removing `-DESP32` from the Arduino component's CMakeLists.txt.
- **INADDR_NONE conflict**: Arduino's `IPAddress.h` declares `INADDR_NONE` as a variable, while lwIP defines it as a macro. Fixed by including Arduino.h before Matter headers in app_main.cpp.
- **Pigweed bootstrap**: May fail on macOS. Critical tools are still installed; the failure only affects host-side build tools.
- **FanControl cluster**: esp-matter v1.0's `fan_control::create()` is not implemented. The FanControl cluster is created manually using low-level attribute APIs.

## License

The CC1101 RF code is licensed under GPLv3 (inherited from the ithowifi project). The Matter integration code is provided as-is.
