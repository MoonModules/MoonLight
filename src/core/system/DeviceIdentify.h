#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>

/// The device-kind enum and its wire labels, the small vocabulary discovery shares.
///
/// @technical
///
/// Shared by the devices module, the plugins, persistence and the UI list; each plugin classifies, and this header carries only the vocabulary, so the enum pulls in no module.

namespace mm {

/// What a discovered device is; Group is a multicast group or the broadcast address a stream goes to, a destination rather than a device.
enum class DevType : uint8_t { Generic = 0, MoonLight = 1, Wled = 2, Hue = 3, Group = 4, MoonBase = 5, Esphome = 6, Tasmota = 7 };

inline const char* devTypeStr(DevType t) {
    switch (t) {
        case DevType::MoonLight: return "MoonLight";
        case DevType::Wled:      return "WLED";
        case DevType::Hue:       return "Hue bridge";
        case DevType::Group:     return "group";
        case DevType::MoonBase:  return "MoonBase";
        case DevType::Esphome:   return "ESPHome";
        case DevType::Tasmota:   return "Tasmota";
        case DevType::Generic:   return "generic";
    }
    return "generic";
}

/// The kind a label names, the inverse of devTypeStr; an unknown label is Generic.
inline DevType devTypeFromStr(const char* s) {
    for (DevType t : {DevType::MoonLight, DevType::Wled, DevType::Hue, DevType::Group, DevType::MoonBase, DevType::Esphome, DevType::Tasmota})
        if (std::strcmp(s, devTypeStr(t)) == 0) return t;
    return DevType::Generic;
}

/// Whether an address is a destination for many rather than one device: a multicast group (224.0.0.0/4) or the broadcast address.
inline bool isGroupAddress(const uint8_t ip[4]) {
    return (ip[0] >= 224 && ip[0] <= 239) || (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255);
}

/// Whether an address is IPv4 link-local, 169.254.0.0/16 (RFC 3927): an interface's own fallback without DHCP, reachable only from its own link.
inline bool isLinkLocal(const uint8_t ip[4]) { return ip[0] == 169 && ip[1] == 254; }

}  // namespace mm
