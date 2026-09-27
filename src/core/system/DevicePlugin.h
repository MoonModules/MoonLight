#pragma once

#include "core/system/DeviceIdentify.h"   // DevType
#include "core/system/WledPacket.h"       // the 65506 presence packet MoonLight + WLED both use

#include <cstdio>
#include <cstring>

/// @defgroup DevicePlugin The device-interop plugin seam
/// @{
/// How a foreign lighting or IoT system hooks into MoonLight's device discovery.
///
/// @moreinfo
///
/// `DevicesModule` owns the device model and the UDP discovery listener and stays domain-neutral, while each plugin teaches it to recognize one ecosystem from a presence broadcast.
/// That is the adapter pattern, the same shape the list-source seam and the factory's register-by-name use.
/// The core is generic and the per-system knowledge lives with its plugin, so a new system is one new file rather than a core edit.
///
/// ## Discovery is passive
///
/// A plugin declares the broadcast port it listens on and classifies a received datagram into a device.
///
/// This replaced an mDNS query path, which destabilized our own mDNS advertising: a query for a service we also host exhausts the IDF mDNS pool.
/// mDNS is now advertise-only, so the WLED app and Home Assistant still find us, and discovery never queries.
///
/// ## Sized for a control half it does not yet have
///
/// The seam covers the discovery half, with two concrete plugins that prove it is not shaped to one system.
/// It is sized to also carry a control half, a per-plugin command that translates something like setting brightness into a system's own protocol, without reshaping.
/// That is why the discovered device stays plain data and the iteration is generic.
///
/// ## Why the MoonLight plugin is listed first
///
/// A MoonLight device broadcasts a WLED-valid packet, so the WLED apps list it, stamped with a sentinel in the version field.
/// Its plugin claims a packet only when that marker is present, and the WLED plugin declines a packet that carries it.
/// Order gives the more specific plugin its chance first.

namespace mm {

/// A device a plugin recognized, the address coming from the datagram and the rest from the plugin.
struct DiscoveredDevice {
    DevType type = DevType::Generic;   ///< which ecosystem it belongs to
    char    name[24] = {};             ///< its display name, truncated to fit
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

    /// The UDP port this ecosystem broadcasts presence on, which every bundled plugin shares.
    virtual uint16_t discoveryPort() const = 0;

    /// Claim and classify a datagram, or decline it so another plugin may try.
    virtual bool classifyPacket(const uint8_t* data, size_t len, const uint8_t srcIp[4],
                                DiscoveredDevice& out) const = 0;

    // Reserved for the control half: a command translated into this system's own protocol.
};

/// Claims a presence packet that carries the MoonLight marker, so a peer is typed as one.
class MmPlugin : public DevicePlugin {
public:
    /// Names this plugin in logs and the UI.
    const char* label() const override { return "MoonLight"; }
    /// The shared presence port.
    uint16_t discoveryPort() const override { return WledPacket::kPort; }

    bool classifyPacket(const uint8_t* data, size_t len, const uint8_t /*srcIp*/[4],
                        DiscoveredDevice& out) const override {
        if (!WledPacket::isValid(data, len) || !WledPacket::hasMmMarker(data, len)) return false;
        out.type = DevType::MoonLight;
        char name[24];
        WledPacket::readName(data, name, sizeof(name));
        setDeviceName(out, name);
        return true;
    }
};

/// Claims any valid WLED presence packet that carries no MoonLight marker.
class WledPlugin : public DevicePlugin {
public:
    /// Names this plugin in logs and the UI.
    const char* label() const override { return "WLED"; }
    /// The shared presence port.
    uint16_t discoveryPort() const override { return WledPacket::kPort; }

    bool classifyPacket(const uint8_t* data, size_t len, const uint8_t /*srcIp*/[4],
                        DiscoveredDevice& out) const override {
        if (!WledPacket::isValid(data, len)) return false;
        if (WledPacket::hasMmMarker(data, len)) return false;   // that's a MoonLight peer
        out.type = DevType::Wled;
        char name[24];
        WledPacket::readName(data, name, sizeof(name));
        setDeviceName(out, name);
        return true;
    }
};

/// @}
}  // namespace mm
