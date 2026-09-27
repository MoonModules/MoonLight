#pragma once

#include <cstdint>

/// @defgroup DeviceIdentify What kind a discovered device is
/// @{
/// The device-kind enum and its wire labels, the small vocabulary discovery shares.
///
/// @moreinfo
///
/// It is shared by the devices module, the interop plugins, persistence and the UI list.
/// A plugin classifies a device straight from its presence packet, so the classification logic lives with each plugin and this header carries only the vocabulary.
/// Keeping it separate is what stops the enum pulling in the module.

namespace mm {

/// What a discovered device is.
enum class DevType : uint8_t { Generic = 0, MoonLight = 1, Wled = 2, Hue = 3 };

inline const char* devTypeStr(DevType t) {
    switch (t) {
        case DevType::MoonLight: return "MoonLight";
        case DevType::Wled:      return "WLED";
        case DevType::Hue:       return "Hue bridge";
        case DevType::Generic:   return "generic";
    }
    return "generic";
}

/// @}
}  // namespace mm
