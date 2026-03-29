
# Develop Overview

* [Installation](https://moonmodules.org/MoonLight/develop/installation/): Setup VSCode, Fork MoonLight
* [Development](https://moonmodules.org/MoonLight/develop/development/): Create branch, change code, document, pull request
* [Nodes](https://moonmodules.org/MoonLight/develop/nodes/): Effects, Modifiers, Layouts and Drivers
* [Live Scripts](https://moonmodules.org/MoonLight/develop/livescripts/): Compilation, execution, sync architecture, external function bindings
* [Modules](https://moonmodules.org/MoonLight/develop/modules/): MoonBase and MoonLight modules
* [Layers](https://moonmodules.org/MoonLight/develop/layers/): Coordinate system Effects and Modifiers run in
* [Sveltekit](https://moonmodules.org/MoonLight/develop/sveltekit/): Explaining the repository MoonLight is forked from
* [Documentation](https://moonmodules.org/MoonLight/develop/documentation/): One repo for code AND documentation

## Utility headers

Pure C++ utilities (no Arduino/ESP32 dependencies) live in `src/MoonBase/utilities/` and can be unit-tested natively (`pio test -e native`):

| Header | Contents |
|---|---|
| `PlatformFunctions.h/.cpp` | ESP32/FreeRTOS helpers: `EXT_LOG*` macros, PSRAM detection, ArduinoJson converters |
| `PureFunctions.h` | Pure C++ only — math helpers, CRC, string utilities. Safe to `#include` in native tests |
| `BoardNames.h` | Board preset name constants and legacy-ID migration. Extracted from `ModuleIO.h` for testability |
| `Char.h` | Fixed-size string wrapper (`Char<N>`) — use instead of `String` in node class members |
| `MemAlloc.h` | `allocMB` / `freeMB` templates for PSRAM-aware allocation |

If you add a utility to an Arduino-dependent header and want to test it natively, follow the `BoardNames.h` extraction pattern: move the pure logic to a new header in `src/MoonBase/utilities/`, include it from the original file, and `#include` it directly in the test.

<a href="https://www.youtube.com/watch?v=tdrU9yGkyVo">
  <img width="380" src="https://img.youtube.com/vi/tdrU9yGkyVo/maxresdefault.jpg" alt="Watch MoonLight Demo">
</a>

**[▶️ Watch the developer quickstart tutorial](https://www.youtube.com/watch?v=tdrU9yGkyVo)**
