#pragma once

#include "core/module/MoonModule.h"
#include "core/util/Hlc.h"   // the fleet's clock, which stamps every edit of a device group

namespace mm {

/// The container for what reaches past this device to the others on the network: the Discovery list, and the groups that join it.
///
/// Fleet-scope rather than this-device, so neither System nor Network: a device's own interfaces live under Network, the devices it finds live here.
///
/// @technical
///
/// It holds no settings of its own, and Discovery is wired in by code, which every boot puts back.
class Devices : public MoonModule {
public:
    /// Accept a device group as a child, added and removed as an effect is.
    const char* acceptsChildRoles() const override { return "devicegroup"; }

    /// The hybrid logical clock this device stamps its group edits with, one for every group, so each reading is newer than any it has seen.
    Hlc& clock() { return clock_; }

private:
    Hlc clock_;
};

} // namespace mm
