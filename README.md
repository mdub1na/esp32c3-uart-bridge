# ESP32-C3 USB–UART bridge

## Contents

- [Destiny](#destiny)
- [Project structure](#project-structure)
- [Stack and tools](#stack-and-tools)
- [Build](#build)
- [Board and operating modes](#board-and-operating-modes)
- [LuatOS CORE with CH343](#luatos-core-with-ch343)
- [Check the bridge on a Mac](#check-the-bridge-on-a-mac)
- [Boundaries](#boundaries)
- [License](#license)

## Destiny

This repository contains ESP32-C3 USB–UART bridge firmware and Mac tools for capturing and checking a device console at 115200 baud, 8N1. The SuperMini V1601 profile uses native USB Serial/JTAG. The LuatOS CORE profile uses the board's CH343 USB interface. Both start with target TX isolated.

Start by [building the profile for your board](#build). The SuperMini loopback build checks USB and internal UART operation with only the board and a USB cable. See [the validation record](docs/mac-validation.md) for results on each board.

## Project structure

```text
├── .github/workflows/    # CI host checks and firmware builds
├── main/                 # SuperMini and LuatOS firmware and input handling
├── tools/                # Serial discovery, loopback, capture and one-shot sender
├── scripts/build.sh      # Build a board profile against the locked SDK
├── tests/                # Host transport and firmware state checks
├── docs/mac-validation.md # Hardware results and untested boundaries
├── reports/              # Sanitized machine-readable hardware evidence
├── idf.lock.json         # ESP-IDF source revision and chip target
├── sdkconfig.defaults    # Normal firmware configuration
├── sdkconfig.loopback    # Explicit internal-loopback diagnostic override
├── sdkconfig.luatos      # LuatOS CORE board override
├── requirements-host.txt # Pinned Python serial dependency
└── LICENSE               # Apache License 2.0 for the project
```

## Stack and tools

| Tool | Purpose in this project |
| --- | --- |
| ESP-IDF 5.5.2 | Native USB Serial/JTAG, UART and FreeRTOS APIs with the revision pinned in `idf.lock.json` |
| C and the RISC-V ESP compiler | Firmware and a native-testable control state machine |
| CMake and Ninja | Firmware configuration and compilation |
| Python 3.9+ and pyserial 3.5 | Host serial discovery, paced binary validation and raw capture |
| esptool, supplied by ESP-IDF | Identify and flash the selected ESP32-C3 |

## Build

Install the [official ESP-IDF prerequisites](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32c3/get-started/linux-macos-setup.html), then obtain the locked SDK:

```sh
git clone --branch v5.5.2 --depth 1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c3
python3 tools/idf_tools.py install cmake ninja
. ./export.sh
```

Run the command for your board and test from this repository.

```sh
sh scripts/build.sh normal    # SuperMini bridge
sh scripts/build.sh loopback  # SuperMini internal test
sh scripts/build.sh luatos    # LuatOS CORE with CH343
```

Each mode keeps a separate build directory and generated configuration. `normal` is the default. The build checks the SDK commit against `idf.lock.json`. Wi-Fi and Bluetooth are disabled.

## Board and operating modes

The `normal` and `loopback` modes use the SuperMini V1601 pinout.

| Connection | Pin |
| --- | --- |
| USB to Mac | Board's USB-C connector; GPIO18/19 remain assigned to USB |
| UART receive | GPIO3, weak internal pull-up to 3.3 V for idle-high |
| UART transmit | GPIO1, disconnected as an output until enabled |
| Local control | BOOT / GPIO9 |
| Status LED | GPIO8, active-low |

This pin choice avoids the default UART0 console pins and boot-strapping pins for the external UART. Compare your board with the [V1601 pin reference](https://nuttx.apache.org/docs/latest/platforms/risc-v/esp32c3/boards/esp32c3-supermini/index.html); SuperMini variants can differ.

Normal firmware receives UART bytes and forwards them unchanged to USB. Host bytes received while TX is disabled are discarded. Hold BOOT for 1.5 seconds to enable transmitting. Press BOOT to disable it again; that same press cannot re-enable transmission. Reset or loss of the USB bus connection disables TX. The USB connection check detects USB host frames, not whether a terminal application has opened the port.

The diagnostic image routes UART internally and does not drive the external TX pin. It automatically enables its test data path. It must not be confused with normal external forwarding.

## LuatOS CORE with CH343

Use the `luatos` image with a CORE board whose USB port enumerates as WCH CH343 (`1a86:55d3`). On this board the onboard CH343 connects to ESP32 UART0 on GPIO20/21. The external UART uses GPIO3 for receive and GPIO1 for transmit. On the header these are labeled `IO03` and `IO01`. [LuatOS CORE board reference](https://wiki.luatos.org/chips/esp32c3/board.html)

| Target connection | LuatOS CORE |
| --- | --- |
| Signal ground | GND |
| Target TX | IO03, with a weak 3.3 V pull-up |
| Target RX | IO01, open-drain transmit with both internal pulls off |

Start by flashing `build/luatos` on the isolated ESP32-C3. For receive-only capture, leave IO01 disconnected and use `capture`. With a verified compatible target RX and idle pull-up, `one-shot` sends a single printable ASCII line after a fresh captured prompt. Replace the example prompt with the full literal prompt, including any trailing space.

```sh
python -m pip install -r requirements-host.txt
python tools/serial_probe.py list
idf.py -B build/luatos -p PORT flash
mkdir -p local
python tools/serial_probe.py capture PORT --output local/console.bin --duration 120
python tools/serial_probe.py one-shot PORT --prompt 'device> ' --line 'help' \
    --output local/one-shot.bin
```

Only an explicit framed request from the host can enable IO01. The firmware allows one valid request per ESP32 boot, checks that the target RX is already high, pulls it low only for the transmit bits, then releases it. The target or a suitable interface must supply the high level. Keep the bridge powered while the target drives connected pins. A successful host write confirms delivery to the adapter port. Inspect the captured target response to confirm execution.

## Check the bridge on a Mac

This procedure checks the SuperMini. Identify its current native USB serial port and use an otherwise disconnected spare board. Flashing replaces its existing application.

```sh
python -m pip install -r requirements-host.txt
python tools/serial_probe.py list
idf.py -B build/loopback -p PORT flash
mkdir -p local
python tools/serial_probe.py loopback PORT --bytes 65536 --timeout 20 \
    --startup-log local/startup.bin --startup-duration 1
idf.py -B build/normal -p PORT flash
```

Replace `PORT` with the identified spare board, for example its `/dev/cu.usbmodem…` path. On the tested Mac, opening the native serial port resets the ESP32 even with DTR/RTS set low before opening. The explicit startup phase preserves the ROM output in a new file on the same connection; it does not silently purge it. Use a new startup filename for another run.

The subsequent diagnostic test compares received bytes with a deterministic binary payload, including zero bytes and control characters. It defaults to 9216 bytes/s and at most 64-byte writes: the USB CDC baud-rate setting does not throttle USB traffic. The test still fails on missing, extra or changed bytes after the startup boundary. See [the validation record](docs/mac-validation.md) for the observed results.

After testing, flash `build/normal` so the board returns to receive-only startup. A successful internal loopback does not establish external pin continuity or voltage compatibility.

## Boundaries

- GPIO signalling is **3.3 V**. Measure the target's UART voltage before connecting it. A 1.8 V target requires appropriate level conversion; this firmware cannot change GPIO voltage.
- RX has a weak 3.3 V pull-up so an unplugged wire does not float and trigger framing errors. Connect it only to the 3.3 V side of a suitable UART or level converter.
- Do not connect the board's 3.3 V or 5 V output to a separately powered target. Establish the target pinout and shared signal ground separately.
- Capture preserves incoming target bytes. The ESP32 ROM can emit startup or reset text on the host port.
- Buffers are finite and there is no external RTS/CTS flow control. The native USB RX driver cannot guarantee backpressure for unlimited host writes. Overflow-free continuous transfer under arbitrary host stalls has not been established.
- UART-to-USB bytes already queued in the USB driver may survive a brief USB disconnection. Reset the bridge between capture sessions that require a fresh boundary.
- Native host tests do not contact boards. Hardware records distinguish internal loopback, external loopback and testing against a real target.

## License

Copyright 2026 mdub1na. The source and documentation are licensed under [Apache-2.0](LICENSE). ESP-IDF and pyserial are available separately under their respective licenses.
