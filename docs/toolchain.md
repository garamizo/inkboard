# Toolchain: why PlatformIO + Arduino (C++)

## Choice

**C++ with the Arduino-ESP32 3.x core, built with PlatformIO** (the
[pioarduino](https://github.com/pioarduino/platform-espressif32) platform).

This is what most ESP32 hobby and small-product firmware uses today. You keep the Arduino
API and libraries you already know (`WiFi`, `HTTPClient`, `GxEPD2`, `ESP_I2S`, ...), but you get:

- **A real project layout.** `src/`, `include/`, `lib/` and `test/`, not a single `.ino` file.
  Multiple `.cpp` files and headers work normally.
- **Reproducible builds.** Platform, core and library versions are pinned in
  `platformio.ini`, so the build is the same on any machine or in CI.
- **CLI and editor.** `pio run` from the terminal, or the PlatformIO extension in VS Code
  with IntelliSense, a serial monitor and a debugger (the C6 has built-in USB-JTAG).
- **Unit tests.** `pio test` runs on the host (`native`) or on the device.
- **Direct ESP-IDF access.** Arduino-ESP32 3.x sits on top of ESP-IDF 5.x, so you can call
  IDF APIs (deep sleep, power management, NVS, OTA, Zigbee) from the same code.

> Why pioarduino and not the stock `espressif32` platform? PlatformIO's official platform
> stopped at Arduino core 2.x, which does not support the ESP32-C6. pioarduino is the
> community fork that tracks Arduino core 3.x.

## Alternatives considered

| Option | Good at | Why not (for now) |
|--------|---------|-------------------|
| **Arduino IDE 2** | Zero setup | Single-sketch layout, no version pinning, weak for a growing codebase. The same code still compiles there if you ever need it. |
| **ESP-IDF (pure C/C++)** | Full control, Espressif's first-class SDK, best for Zigbee/Thread | More boilerplate (CMake, menuconfig, FreeRTOS tasks everywhere), and the e-paper libraries are Arduino-based. Can be adopted piecemeal through Arduino-as-component. |
| **ESPHome (YAML)** | Great if the dashboard is just a Home Assistant view | Hard to do custom layouts and logic; ties the device to HA. |
| **MicroPython** | Fast iteration in a REPL | Slower and memory-hungry for an 800×480 frame buffer; weaker deep-sleep and power control. |
| **Rust (esp-hal / esp-idf-svc)** | Memory safety, modern tooling | Smaller ecosystem for e-paper and audio; steep learning curve. |

## Setup

```bash
uv tool install platformio          # or: pipx install platformio
sudo usermod -aG dialout $USER      # serial port access; log out and back in afterwards
```

In VS Code, install the recommended **PlatformIO IDE** extension (the repo suggests it).

## Everyday commands

```bash
pio run                     # build
pio run -t upload           # build + flash over USB-C
pio device monitor          # serial console (Ctrl+C to exit)
pio run -t upload -t monitor
pio run -t clean
```

The first build downloads the RISC-V toolchain and Arduino core (~1 GB into `~/.platformio`).
