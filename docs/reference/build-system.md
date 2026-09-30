# Build system

How the source tree maps onto CMake, and where each platform's entry point lives. The steps to build are [Building, running, flashing](../how-to/building.md); this is the layout to look up.

CMake is the sole build system. The source tree is shared across every platform, but build entry points are separate because ESP-IDF wraps CMake with its own conventions (`idf_component_register()` instead of `add_library()`).

```text
CMakeLists.txt                          ← standard CMake: desktop / RPi + tests
src/
  main.cpp                              ← shared pipeline wiring (mm_main), platform-neutral
  platform/
    desktop/
      main_desktop.cpp                  ← desktop entry point: int main() + SIGINT
      platform_config.h                 ← desktop platform constants
    esp32/
      platform_config.h                 ← ESP32 platform constants (reads sdkconfig)
esp32/
  CMakeLists.txt                        ← ESP-IDF project root (thin wrapper)
  main/
    CMakeLists.txt                      ← idf_component_register() pointing at src/
    main.cpp                            ← ESP32 entry point: app_main() + Ethernet init
  sdkconfig.defaults                    ← board-specific defaults
```

The shared `src/main.cpp` defines `mm_main(keepRunning, gridW, gridH)`, the full pipeline wiring. Each platform provides a thin entry point that does platform-specific init (SIGINT on desktop, Ethernet on ESP32) then calls `mm_main()`.

The project is structured as a small set of CMake libraries: a core library (platform-independent), a platform library (selected at configure time), an application target (links both, provides the entry point). Further decomposition (effects, networking, drivers as separate libraries) happens when the codebase is large enough to justify it.

Which script runs which build: [MoonDeck.md](../../moondeck/MoonDeck.md). Why the tooling is ours rather than PlatformIO: [Why we write our own code](../explanation/why-we-write-our-own.md).

## Source

[CMakeLists.txt](https://github.com/MoonModules/MoonLight/blob/main/CMakeLists.txt) is the desktop and test build; [esp32/CMakeLists.txt](https://github.com/MoonModules/MoonLight/blob/main/esp32/CMakeLists.txt) is the ESP-IDF project root.
