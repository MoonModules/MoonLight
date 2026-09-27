#pragma once

#include "core/module/MoonModule.h"
#include "core/system/NetworkModule.h"
#include "core/system/SystemModule.h"
#include "core/system/HttpServerModule.h"
#include "core/util/build_info.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace mm {

/// Browser-driven WiFi provisioning over serial, using the open Improv protocol.
///
/// It solves the bootstrap problem: a freshly flashed board is not yet on your network.
/// Improv carries that first handoff over the cable the browser already used to flash it.
/// This module is the status surface, the protocol living in the platform layer.
/// @card ImprovProvisioningModule.png
///
/// @moreinfo
///
/// ## Transports
///
/// The listener serves an external USB-to-UART bridge and a native USB port alike.
/// Without either, the access-point flow remains, the device serving its own network.
///
/// ## The commands
///
/// Four standard commands report state, scan for networks, and set credentials.
/// Two vendor commands extend them.
/// One caps transmit power before any association, for a board whose supply browns out.
/// The other carries one config operation as JSON, routed to the same apply-core.
/// So a call over the network and one over serial execute identically.
/// That is what lets the installer configure a device a browser cannot reach directly.
///
/// ## Applying an operation
///
/// Operations are idempotent, and a long value chunks across frames into a buffer.
/// It is applied on the main loop rather than the serial task, one at a time.
/// A failure cannot travel back on the spent acknowledgement, so it surfaces here.
class ImprovProvisioningModule : public MoonModule {
public:
    /// Adopt the system module, whose name and version the device info reports.
    void setSystemModule(SystemModule* s) { systemModule_ = s; }
    /// Adopt the network module, which receives the credentials this one collects.
    void setNetworkModule(NetworkModule* n) { networkModule_ = n; }
    /// Adopt the web server, whose apply-core a pushed operation is routed to.
    void setHttpServerModule(HttpServerModule* h) { httpServerModule_ = h; }

    /// Keep listening whatever the toggle says, as the other fixed services do.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// Apparatus rather than content, so it cannot be deleted, only disabled.
    bool userEditable() const override { return false; }

    /// Install the serial listener, handing it the buffers the protocol task writes.
    void setup() override {
        if constexpr (platform::hasImprov) {
            // Borrowed: the task copies them on init, so these locals may expire.
            const char* deviceName = systemModule_ ? systemModule_->deviceName() : "MoonLight";
            platform::ImprovDeviceInfo info{
                deviceName,
                platform::chipModel(),
                kVersion,
            };
            platform::improvProvisioningInit(
                info,
                pendingSsid_, sizeof(pendingSsid_),
                pendingPassword_, sizeof(pendingPassword_),
                &pendingCredentials_,
                statusStr_, sizeof(statusStr_),
                &pendingTxPower_, &pendingTxPowerReady_,
                pendingOp_, sizeof(pendingOp_), &pendingOpReady_);
        } else {
            std::strncpy(statusStr_, "not supported on this platform", sizeof(statusStr_) - 1);
        }
    }

    /// Declare the one status readout.
    void defineControls() override {
        controls_.addReadOnly("provision_status", statusStr_, sizeof(statusStr_));
    }

    /// Poll what the protocol task published: the power cap first, then any credentials.
    void tick1s() MM_NONBLOCKING override {
        // First on purpose: the cap must land before the association attempt.
        if (pendingTxPowerReady_.load(std::memory_order_acquire) && networkModule_) {
            networkModule_->setTxPowerSetting(pendingTxPower_);
            pendingTxPowerReady_.store(false, std::memory_order_release);
        }
        // The acquire pairs with the task's release, so the buffer writes are visible here.
        if (pendingCredentials_.load(std::memory_order_acquire) && networkModule_) {
            networkModule_->setWifiCredentials(pendingSsid_, pendingPassword_);
            // Wipe the password; the name is not sensitive and may serve a re-provision.
            std::memset(pendingPassword_, 0, sizeof(pendingPassword_));
            pendingCredentials_.store(false, std::memory_order_release);
        }
        // The device model arrives like any other catalog default, through the operation below.
    }

    /// Apply any pending operation, polled every tick so a burst of them installs briskly.
    void tick() MM_NONBLOCKING override {
        if (pendingOpReady_.load(std::memory_order_acquire) && httpServerModule_) {
            // The frame was acknowledged on receipt, so a failure must surface here instead.
            auto r = httpServerModule_->applyOp(pendingOp_);
            if (r != HttpServerModule::OpResult::Ok &&
                r != HttpServerModule::OpResult::AlreadyExists) {
                std::printf("Improv APPLY_OP failed (result=%d): %s\n",
                            static_cast<int>(r), pendingOp_);
                std::snprintf(statusStr_, sizeof(statusStr_), "error: apply failed (%d)",
                              static_cast<int>(r));
            }
            std::memset(pendingOp_, 0, sizeof(pendingOp_));
            pendingOpReady_.store(false, std::memory_order_release);
        }
        MoonModule::tick();   // tick children (none today, but keep the contract)
    }

private:
    SystemModule*     systemModule_     = nullptr;
    NetworkModule*    networkModule_    = nullptr;
    HttpServerModule* httpServerModule_ = nullptr;
    char statusStr_[64] = "listening";   ///< what the one control reports

    // Published with a release, which the acquire above pairs with across cores.
    char pendingSsid_[33] = {};              ///< the network to join
    char pendingPassword_[64] = {};          ///< wiped once handed on
    std::atomic<bool> pendingCredentials_{false};   ///< set when both are ready

    uint8_t pendingTxPower_ = 0;             ///< the cap, in whole dBm
    std::atomic<bool> pendingTxPowerReady_{false};  ///< set when it is ready

    char pendingOp_[512] = {};               ///< one operation, sized for the longest
    std::atomic<bool> pendingOpReady_{false};       ///< set when it is reassembled
};

} // namespace mm
