#pragma once

/// A responder the host lends the host platform's mDNS calls: an app shell that has one sets it, as the Android app does with NsdManager through main_android.cpp. Unset, the calls only remember what a test reads back.

#include <cstdint>

namespace mm::platform {

/// The responder's calls, each null where the host lends none.
struct HostMdns {
    bool (*begin)(const char* deviceName) = nullptr;                          ///< announce the device and its page under this name, again on a rename
    void (*advertise)(const char* type, const char* proto, uint16_t port) = nullptr;   ///< announce one more service, such as `_apple-midi._udp`
    void (*withdraw)(const char* type, const char* proto) = nullptr;          ///< withdraw one service
    void (*end)() = nullptr;                                                  ///< withdraw everything
};

/// Lend the platform a responder, before the firmware starts.
void setHostMdns(const HostMdns& responder);

}  // namespace mm::platform
