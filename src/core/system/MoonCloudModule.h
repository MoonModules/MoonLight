// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once


#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/module/MoonModule.h"
#include "core/util/sha256.h"
#include "platform/platform.h"

namespace mm {

/// The container for everything MoonLight does with a server we run.
///
/// It holds no controls and does no work of its own.
/// Each thing MoonCloud does is a child with its own consent: Stats, Talk, and Sync to come.
/// A user who wants a joint lightshow has not agreed to usage reporting.
class MoonCloudModule : public MoonModule {
public:
    /// Always runs: the children carry the real consent, so disabling the container is ambiguous.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// How long an outbound call may take before it is abandoned.
    static constexpr uint32_t kTimeoutMs = 4000;

    /// The one outbound call, over HTTPS alone since the address is a compiled-in constant.
    bool post(const char* path, const char* body) const {
        char url[192];
        std::snprintf(url, sizeof(url), "https://%s%s", kHost, path);
        return platform::httpsPost(url, body, kTimeoutMs);
    }



private:
    // Compiled in rather than configurable; this name because a custom domain's CA is absent.
    static constexpr const char* kHost = "mooncloud-stats.moonmodules.workers.dev";
};


/// @defgroup MoonCloudIdentity The installation id, a salted hash of the MAC
/// @{
///
/// Pseudonymous, and salted apart from the MQTT and Home Assistant ids.

/// Characters written by `installationId`, excluding the terminator.
inline constexpr size_t kInstallationIdChars = 32;

/// Changing this re-identifies every installation in the world exactly once, so the line carries a marker the rename sweep honours.
inline constexpr char kMoonCloudSalt[] = "projectMM/MoonStats/v1";   // rename-keep: a new salt orphans every installation's history

/// Write the installation id into `out`, which must hold one more character than the id.
inline void installationId(char* out) {
    if (!out) return;

    uint8_t mac[6] = {};
    platform::getMacAddress(mac);

    // Concatenation rather than HMAC: the salt is public either way, so it separates rather than keys.
    uint8_t buf[sizeof(kMoonCloudSalt) - 1 + sizeof(mac)];
    std::memcpy(buf, kMoonCloudSalt, sizeof(kMoonCloudSalt) - 1);
    std::memcpy(buf + sizeof(kMoonCloudSalt) - 1, mac, sizeof(mac));

    sha256Hex(buf, sizeof(buf), out, kInstallationIdChars / 2);
}

/// @}

}  // namespace mm
