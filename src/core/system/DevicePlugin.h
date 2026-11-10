#pragma once

#include "core/system/DeviceIdentify.h"   // DevType
#include "core/system/WledPacket.h"       // the 65506 presence packet MoonLight + WLED both use

#include <cstdio>
#include <cstring>

/// How a foreign lighting or IoT system hooks into MoonLight's device discovery.
///
/// @technical
///
/// `DevicesModule` owns the device model and the listener, and each plugin recognizes one ecosystem from a presence broadcast: the adapter pattern, so a new system is one new file.
///
/// ## Discovery is passive
///
/// A plugin declares the broadcast port it listens on and classifies a received datagram into a device.
///
/// Discovery never queries mDNS: a query for a service this device also hosts exhausts the IDF mDNS pool. mDNS stays advertise-only, so the WLED app and Home Assistant find us.
///
/// ## Sized for a control half it does not yet have
///
/// The seam is sized to also carry a per-plugin command, such as a brightness set in a system's own protocol, which is why the discovered device stays plain data.
///
/// ## Why the MoonLight plugin is listed first
///
/// A MoonLight packet is WLED-valid with a sentinel; its plugin claims only a marked packet and the WLED plugin declines one, so order gives the specific plugin its chance first.

namespace mm {

/// A device a plugin recognized, the address coming from the datagram and the rest from the plugin.
struct DiscoveredDevice {
    DevType type = DevType::Generic;   ///< which ecosystem it belongs to
    char    name[24] = {};             ///< its display name, truncated to fit
    uint16_t httpPort = WledPacket::kDefaultHttpPort;   ///< the port its web page answers on
};

/// Copy a name in, truncating explicitly so the read is bounded.
inline void setDeviceName(DiscoveredDevice& d, const char* src) {
    std::snprintf(d.name, sizeof(d.name), "%.*s",
                  static_cast<int>(sizeof(d.name) - 1), src ? src : "");
}

/// One interop plugin: a stateless singleton turning a presence datagram into a device.
class DevicePlugin {
public:
    /// Destroyed through this interface, since the module holds plugins by base pointer.
    virtual ~DevicePlugin() = default;

    /// A short label for logs and the UI, with flash-literal lifetime.
    virtual const char* label() const = 0;

    /// The UDP port this ecosystem broadcasts presence on, which every bundled plugin shares: @xref{discovery-is-passive}.
    virtual uint16_t discoveryPort() const = 0;

    /// Claim and classify a datagram, or decline it so another plugin may try.
    virtual bool classifyPacket(const uint8_t* data, size_t len, const uint8_t srcIp[4],
                                DiscoveredDevice& out) const = 0;

    // Reserved for the control half: @xref{sized-for-a-control-half-it-does-not-yet-have}.
};

/// A plugin reading the WLED presence packet, which MoonLight and WLED share: its port, and the name it carries.
class PresencePlugin : public DevicePlugin {
public:
    /// The shared presence port.
    uint16_t discoveryPort() const override { return WledPacket::kPort; }

protected:
    /// Claim a valid packet as `type`, with the name it carries.
    static bool claim(const uint8_t* data, DevType type, DiscoveredDevice& out) {
        out.type = type;
        char name[24];
        WledPacket::readName(data, name, sizeof(name));
        setDeviceName(out, name);
        return true;
    }
};

/// Claims a presence packet that carries the MoonLight marker, so a peer is typed as one: @xref{why-the-moonlight-plugin-is-listed-first}.
class MmPlugin : public PresencePlugin {
public:
    /// Names this plugin in logs and the UI.
    const char* label() const override { return "MoonLight"; }
    bool classifyPacket(const uint8_t* data, size_t len, const uint8_t /*srcIp*/[4],
                        DiscoveredDevice& out) const override {
        if (!WledPacket::isValid(data, len) || !WledPacket::hasMmMarker(data, len)) return false;
        out.httpPort = WledPacket::httpPort(data, len);
        return claim(data, DevType::MoonLight, out);
    }
};

/// Claims any valid WLED presence packet that carries no MoonLight marker.
class WledPlugin : public PresencePlugin {
public:
    /// Names this plugin in logs and the UI.
    const char* label() const override { return "WLED"; }
    bool classifyPacket(const uint8_t* data, size_t len, const uint8_t /*srcIp*/[4],
                        DiscoveredDevice& out) const override {
        if (!WledPacket::isValid(data, len) || WledPacket::hasMmMarker(data, len)) return false;   // a marked one is a MoonLight peer
        return claim(data, DevType::Wled, out);
    }
};

}  // namespace mm
