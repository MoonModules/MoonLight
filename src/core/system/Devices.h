#pragma once

#include "core/module/MoonModule.h"
#include "core/system/FleetClock.h"   // the fleet clock, kept on the lowest live device's
#include "core/util/Hlc.h"   // the fleet's clock, which stamps every edit of a device group
#include "core/util/WallPlace.h"   // this device's place in a wall, and the wall its groups make

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

    /// The fleet clock, which Discovery offers the live peers each second.
    FleetClock& fleetClock() { return fleetClock_; }
    /// The same, read-only, for a status.
    const FleetClock& fleetClock() const { return fleetClock_; }

    /// This device's place in a wall, which its light domain sets, unplaced where the device is no part of one.
    void setPlace(const WallPlace& p) MM_NONBLOCKING { place_ = p; }
    /// The same, which the group sync sends to the members.
    const WallPlace& place() const MM_NONBLOCKING { return place_; }
    /// The wall this device's groups make, the bounding box of its own place and its members' heard ones, unplaced while its own is.
    const WallPlace& wall() const MM_NONBLOCKING { return wall_; }
    /// Set the wall, which the group sync derives each second.
    void setWall(const WallPlace& w) MM_NONBLOCKING { wall_ = w; }

    /// Start the fleet clock's task, which opens its socket once the network is up.
    void prepare() override {
        MoonModule::prepare();
        fleetClock_.start();
    }

    /// Stop the fleet clock's task and close its socket.
    void release() override {
        fleetClock_.close();
        MoonModule::release();
    }

private:
    Hlc clock_;
    FleetClock fleetClock_;
    WallPlace place_;   ///< this device's place in a wall
    WallPlace wall_;    ///< the wall its groups make
};

} // namespace mm
