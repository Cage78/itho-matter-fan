# Itho Matter Fan

Native Matter support for Itho Daalderop HRU 300 WTW ventilation units, eliminating the need for Homebridge or other bridges.

Runs directly on the ESP32 add-on board (ithowifi project by arjenhiemstra) with a CC1101 RF module. Exposes the ventilation unit as a Matter **Air Purifier** (the closest native device type for a WTW unit) with 3 speed settings (Low, Medium, High — the HRU 300 cannot be turned off), native HEPA filter status, four temperature sensors, and a Summer Night Boost switch.

> **Stack**: esp-matter release/v1.5 (Matter 1.5.1) + ESP-IDF v5.5.5 + arduino-esp32 3.3.12.
> The legacy esp-matter v1.0 / IDF v4.4.7 version is preserved in the `esp-matter-v1.0` branch.

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

### Filter Status

The Air Purifier endpoint carries a **HEPA Filter Monitoring** cluster. The HRU 300 signals "clean the filters" (W01, blinking status LED) as error number 1 in the 0x2401 status; it does not answer the CVE-era 0x31D9 filter query. The mapping is:

- Condition: 100 (OK) / 0 (dirty) — the unit reports no lifetime percentage
- ChangeIndication: OK / Critical

A bridged **contact sensor** named "HRU Filter" carries the same state as a fallback for controllers that don't render filter status (open = filters need cleaning; note Apple Home shows Boolean State true as "Closed", so the value is inverted).

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

### 1. ESP-IDF v5.5.5

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5.5
cd esp-idf-v5.5.5
./install.sh esp32
```

### 2. esp-matter release/v1.5

```bash
mkdir -p ~/git && cd ~/git
git clone -b release/v1.5 --recursive https://github.com/espressif/esp-matter.git esp-matter-v1.5-clone
cd esp-matter-v1.5-clone
python connectedhomeip/connectedhomeip/scripts/checkout_submodules.py --shallow --platform esp32
```

**Note:** keep the checkout outside iCloud-synced folders (not under `~/Documents`); macOS "Optimize Mac Storage" can evict files mid-build and corrupt the tree.

### 3. Pigweed bootstrap (Matter SDK build tools)

Run with the IDF 5.5 Python environment first on PATH (the bootstrap fails on an old system Python):

```bash
cd ~/git/esp-matter-v1.5-clone/connectedhomeip/connectedhomeip
export PATH="$HOME/.espressif/python_env/idf5.5_py3.12_env/bin:$PATH"
bash scripts/bootstrap.sh
```

Verify that `gn` and `ninja` landed in `.environment/cipd/packages/pigweed`.

### 4. Arduino-ESP32 3.3.12 (CC1101 SPI dependency)

The CC1101 RF code uses Arduino's SPI library. `build.sh` clones it into `components/arduino` automatically and applies a small patch (see below) — no manual step needed.

## Build

```bash
cp main/local_config.example.h main/local_config.h   # set your remote ID, see below
./build.sh build
```

The script sets up the full environment (IDF paths, toolchain, gn/ninja), clones+patches Arduino if missing, and runs `idf.py` from the repo root. No copying into the esp-matter tree is needed; `ESP_MATTER_PATH` and `IDF_PATH` can be overridden via the environment.

### Flash

```bash
./build.sh -p /dev/cu.usbserial-XXXX flash monitor
```

## Binary Size

| Component | Size |
|---|---|
| itho_fan.bin | 1.45 MB |
| App partition | 1.88 MB |
| Free space | 442 KB (23%) |

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
2. Trigger a join command from the ESP32 (fail safe button on GPIO 32)
3. The HRU 300 will register the virtual remote ID

If you copied the remote ID from an existing physical remote, it should already be registered and no joining is needed.

### Matter Commissioning

After flashing, commission the device using a Matter controller (Apple Home, Google Home, etc.):

1. Open your Matter controller app
2. Add a new device
3. Scan the Matter QR code (or use the manual pairing code from serial output)
4. The device will join your WiFi network and appear as an air purifier with sensors

## Project Structure

```
itho-matter-fan/
├── README.md                  # This file
├── build.sh                   # Build script with environment setup + arduino bootstrap
├── CMakeLists.txt             # ESP-IDF project CMakeLists
├── partitions.csv             # 4MB flash partition table (dual OTA)
├── sdkconfig.defaults         # ESP-IDF default config (clusters, FreeRTOS 1000Hz, OTA)
├── patches/
│   └── arduino-3.3.12-wrap-gating.patch  # Gate arduino's --wrap flags on its Matter lib
├── components/arduino/        # arduino-esp32 3.3.12 (auto-cloned by build.sh, gitignored)
└── main/
    ├── CMakeLists.txt         # Main component CMakeLists
    ├── app_main.cpp           # Matter Air Purifier + CC1101 + I2C integration
    ├── local_config.example.h # Template for the remote ID
    └── cc1101/                # CC1101 RF driver (from ithowifi project)
        ├── CC1101.cpp         # CC1101 SPI driver
        ├── CC1101.h
        ├── CC1101Packet.h
        ├── IthoCC1101.cpp     # Itho RF protocol layer
        ├── IthoCC1101.h
        ├── IthoPacket.h       # Itho command definitions
        └── sys_log.h          # Minimal logging stub (maps to ESP_LOG)
```

## Credits

- **CC1101 RF code**: From the [ithowifi project](https://github.com/arjenhiemstra/ithowifi) by arjenhiemstra (GPLv3)
- **esp-matter**: [Espressif Matter SDK](https://github.com/espressif/esp-matter) release/v1.5
- **Arduino-ESP32**: [Espressif Arduino core](https://github.com/espressif/arduino-esp32) 3.3.12
- **ESP-IDF**: [Espressif IoT Development Framework](https://github.com/espressif/esp-idf) v5.5.5

## Known Issues

- **Arduino Matter library clash**: Arduino-ESP32 3.3.x ships its own Matter library which includes `esp_matter.h` and adds `--wrap` link flags whose implementations only exist in the registry version of esp-matter. We compile Arduino with `CONFIG_ARDUINO_SELECTIVE_Matter=n` and gate the wrap flags (`patches/arduino-3.3.12-wrap-gating.patch`, applied automatically by `build.sh`).
- **sdkconfig.defaults changes require deleting `sdkconfig`**: defaults only apply on a fresh configure. If you edit `sdkconfig.defaults`, remove the `sdkconfig` file before rebuilding.
- **Arduino `-DESP32` conflict**: Arduino-ESP32 defines `-DESP32` as a preprocessor macro, which conflicts with the Matter SDK's use of `ESP32` in include paths. Fixed by removing `-DESP32` from the Arduino component's CMakeLists.txt (v1.0 branch; no longer needed on 3.3.x).
- **INADDR_NONE conflict**: Arduino's `IPAddress.h` declares `INADDR_NONE` as a variable, while lwIP defines it as a macro. Fixed by including Arduino.h before Matter headers in app_main.cpp.

## License

The CC1101 RF code is licensed under GPLv3 (inherited from the ithowifi project). The Matter integration code is provided as-is.
