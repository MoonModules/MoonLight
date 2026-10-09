#pragma once

#include "core/util/DeviceName.h"   // defaultDeviceName: shared with MoonBase
#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/system/FilesystemModule.h"   // setDeviceModel() arms the debounced save (noteDirty)
#include "core/util/build_info.h"        // kFirmwareName: the variant persisted for MoonBase
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// The device's identity and its vitals, always loaded and always visible.
///
/// It owns the network name and the hardware model, and surfaces the live tick metrics.
///
/// Prior art: MoonLight's system diagnostics over REST, with the device name driving mDNS.
/// @card SystemModule.png
///
/// @moreinfo
///
/// ## What it reports
///
/// The dynamic readings refresh every second: uptime, frames a second, tick time, heap and PSRAM.
/// The static ones are read once at boot: chip, CPU, SDK, flash and the reset reason.
///
/// PSRAM is derived rather than flagged, since the allocator merges its pool at boot.
/// A total larger than the internal total is the signal, so a board without it adds nothing.
/// The co-processor readout appears only where the radio is a separate chip.
///
/// ## The two identities
///
/// `deviceName` is the one network identity, behind mDNS, the SoftAP SSID and DHCP.
/// It is coerced to a valid hostname every tick, so a rename propagates within one tick.
///
/// `deviceModel` is the physical board, an entry from the device-model catalog.
/// A device cannot identify its own hardware, so tooling pushes this one.
/// A validator on the control checks every write path rather than each transport doing so.
class SystemModule : public MoonModule {
public:
    /// Adopt the scheduler, whose timings the vitals read.
    void setScheduler(Scheduler* s) { scheduler_ = s; }

    /// Keep the diagnostics ticking, since hiding uptime and heap serves nobody.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    // It accepts no user-added children: its own are wired by code.

    /// Settle the device name, prime the static readouts, and apply the saved log level.
    void setup() override {
        // Every network name derives from this, so it is valid before the network reads it.
        sanitizeHostname(deviceName_);
        if (deviceName_[0] == 0) {
            uint8_t mac[6];
            platform::getMacAddress(mac);
            defaultDeviceName(mac, deviceName_, sizeof(deviceName_));
        }

        // The static strings bind straight to the platform's own storage, with no copy here.
        if constexpr (platform::hasWifiCoprocessor) {
            (void)platform::coprocessorWifi();   // prime the static (the control points at it)
        }

        if (chipFlashVal_ > 0) {
            std::snprintf(flashStr_, sizeof(flashStr_), "%uMB",
                          static_cast<unsigned>(chipFlashVal_ / (1024 * 1024)));
        }

        // Now, so a device that booted quiet is quiet from its first tick.
        applyLogLevel();

        MoonModule::setup();
    }

    /// The persisted log level, which the main loop reads to gate its once-a-second line.
    platform::LogLevel logLevel() const { return static_cast<platform::LogLevel>(logLevel_); }

    /// Apply a log-level change live, and re-assert the firmware variant a load overwrote.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "logLevel") == 0) applyLogLevel();
        // A load overwrites the variant with the previous image's, so the constant re-asserts.
        if (std::strcmp(controlName, "firmware") == 0
            && std::strcmp(firmwareVariant_, kFirmwareName) != 0) {
            std::snprintf(firmwareVariant_, sizeof(firmwareVariant_), "%s", kFirmwareName);
            markDirty();
        }
    }

    /// Declare the identity controls, the live gauges and the static readouts.
    void defineControls() override {
        // Queried here, so the conditionals gating the gauges below see real values.
        totalInternalVal_ = static_cast<uint32_t>(platform::totalInternalHeap());
        totalHeapVal_ = static_cast<uint32_t>(platform::totalHeap());
        chipFlashVal_ = static_cast<uint32_t>(platform::flashChipSize());

        // Device name on top
        controls_.addText("deviceName", deviceName_, sizeof(deviceName_));

        // Text because that is what persistence saves, and seeded only when empty.
        if (deviceModel_[0] == 0) {
            std::snprintf(deviceModel_, sizeof(deviceModel_), "%s", platform::hostPlatform());
        }
        controls_.addText("deviceModel", deviceModel_, sizeof(deviceModel_), validateDeviceModel);

        // Written from the constant each boot, and persisted only so the recovery image reads it.
        std::snprintf(firmwareVariant_, sizeof(firmwareVariant_), "%s", kFirmwareName);
        controls_.addText("firmware", firmwareVariant_, sizeof(firmwareVariant_));
        controls_.setHidden(controls_.count() - 1, true);   // FirmwareUpdateModule's card shows it
        controls_.setReadOnly(controls_.count() - 1, true);

        // Dynamic (updated every second)
        controls_.addReadOnly("uptime", uptimeStr_, sizeof(uptimeStr_));
        controls_.addReadOnly("fps", fpsStr_, sizeof(fpsStr_));
        controls_.addReadOnly("tickTimeUs", tickStr_, sizeof(tickStr_));
        if (totalInternalVal_ > 0) {
            controls_.addProgress("heap", heapUsedVal_, totalInternalVal_);
        }
        // A total larger than the internal total is the PSRAM signal, with no per-platform branch.
        if (totalHeapVal_ > totalInternalVal_) {
            controls_.addProgress("psram", psramUsedVal_, totalHeapVal_ - totalInternalVal_);
            // A compile-time string, so it binds read-only and adds nothing on a board without it.
            if (const char* pt = platform::psramType(); pt && pt[0])
                controls_.addReadOnly("psramType", const_cast<char*>(pt));
        }
        controls_.addReadOnly("maxBlock", maxBlockStr_, sizeof(maxBlockStr_));

        // Firmware identity lives on the firmware card, and filesystem usage on the file manager.
        if (chipFlashVal_ > 0) {
            controls_.addReadOnly("flash", flashStr_, sizeof(flashStr_));
        }

        // These never change at runtime, so each binds straight to the platform's own buffer.
        controls_.addReadOnly("mac", const_cast<char*>(platform::macString()));   ///< tooling keys on this
        controls_.addReadOnly("chip", const_cast<char*>(platform::chipModel()));
        controls_.addReadOnly("cpu", const_cast<char*>(platform::cpuInfo()));   ///< the clock and core count
        controls_.addReadOnly("sdk", const_cast<char*>(platform::sdkVersion()));
        controls_.addReadOnly("bootReason", const_cast<char*>(platform::resetReason()));
        // The UI honors this client-side, so nothing in the firmware reads it. A LEVEL rather than a switch, because a control names the audience it is for and one number decides which audience is reading.
        controls_.addSelect("mode", mode_, kModeOptions, 3);
        // Warn keeps the once-a-second line off the wire while warnings still print.
        controls_.addSelect("logLevel", logLevel_, logLevelOptions_, 6);
        controls_.setDeveloper(controls_.count() - 1);
        // Compiled out where the radio is native, so the control and its query both vanish.
        if constexpr (platform::hasWifiCoprocessor) {
            controls_.addReadOnly("wifiCoproc", const_cast<char*>(platform::coprocessorWifi()));
        }

        MoonModule::defineControls();
    }

    /// Coerce the device name, then refresh every live reading.
    void tick1s() MM_NONBLOCKING override {
        // It runs before the network module reads the name, so a rename propagates in one tick.
        sanitizeHostname(deviceName_);
        if (deviceName_[0] == 0) {        // cleared or all-invalid, so fall back to the MAC
            uint8_t mac[6];
            platform::getMacAddress(mac);
            defaultDeviceName(mac, deviceName_, sizeof(deviceName_));
        }

        // Update dynamic values
        uint32_t uptimeSec = scheduler_ ? scheduler_->elapsed() / 1000 : 0;
        uint32_t hours = uptimeSec / 3600;
        uint32_t mins = (uptimeSec % 3600) / 60;
        uint32_t secs = uptimeSec % 60;
        mm::formatTo(uptimeStr_, sizeof(uptimeStr_), "%u:%02u:%02u",
                      static_cast<unsigned>(hours),
                      static_cast<unsigned>(mins),
                      static_cast<unsigned>(secs));

        uint32_t fps = scheduler_ ? scheduler_->fps() : 0;
        mm::formatTo(fpsStr_, sizeof(fpsStr_), "%u", static_cast<unsigned>(fps));

        uint32_t tickUs = scheduler_ ? scheduler_->tickTimeUs() : 0;
        mm::formatTo(tickStr_, sizeof(tickStr_), "%u", static_cast<unsigned>(tickUs));

        uint32_t freeTotal = static_cast<uint32_t>(platform::freeHeap());
        uint32_t freeInternal = static_cast<uint32_t>(platform::freeInternalHeap());
        heapUsedVal_ = totalInternalVal_ > freeInternal ? totalInternalVal_ - freeInternal : 0;
        uint32_t freePsram = freeTotal > freeInternal ? freeTotal - freeInternal : 0;
        uint32_t totalPsram = totalHeapVal_ > totalInternalVal_ ? totalHeapVal_ - totalInternalVal_ : 0;
        psramUsedVal_ = totalPsram > freePsram ? totalPsram - freePsram : 0;

        // The internal block, not the all-memory one, which reads as megabytes on a PSRAM board.
        mm::formatTo(maxBlockStr_, sizeof(maxBlockStr_), "%uKB",
                      static_cast<unsigned>(platform::maxInternalAllocBlock() / 1024));

        // The control points straight at the platform's buffer, so calling it is the refresh.
        if constexpr (platform::hasWifiCoprocessor) {
            (void)platform::coprocessorWifi();
        }

        MoonModule::tick1s();
    }

    /// The network identity, guaranteed a valid non-empty hostname since it is coerced each tick.
    const char* deviceName() const { return deviceName_; }

    /// The physical-hardware identity (device-model catalog entry name), pushed by tooling.
    const char* deviceModel() const { return deviceModel_; }

    /// Accept printable ASCII within the buffer, which every write path checks.
    static bool validateDeviceModel(const char* value) {
        if (!value) return false;
        size_t n = std::strlen(value);
        if (n == 0 || n >= 32) return false;   // 1..31 (32-byte buffer, NUL-terminated)
        for (size_t i = 0; i < n; i++) {
            unsigned char b = static_cast<unsigned char>(value[i]);
            if (b < 0x20 || b > 0x7E) return false;
        }
        return true;
    }

private:
    Scheduler* scheduler_ = nullptr;

    char deviceName_[24] = {};   ///< the one network identity
    uint8_t mode_ = 0;           ///< one level the whole UI composes against: user, expert, developer
    static constexpr const char* kModeOptions[3] = {"user", "expert", "developer"};
    /// Push the level to the logger, clamped so a corrupt value cannot index past the end.
    void applyLogLevel() {
        uint8_t lvl = logLevel_ > static_cast<uint8_t>(platform::LogLevel::Verbose)
                          ? static_cast<uint8_t>(platform::LogLevel::Verbose) : logLevel_;
        platform::setLogLevel(static_cast<platform::LogLevel>(lvl));
    }

    // The first 60 s always logs at Info regardless, which is when the installer reads the IP.
    uint8_t logLevel_ = static_cast<uint8_t>(platform::LogLevel::Warn);
    static constexpr const char* logLevelOptions_[] = {"None", "Error", "Warn", "Info", "Debug", "Verbose"};
    char deviceModel_[32] = {};   ///< the catalog entry name, sized to the longest with headroom
    char     firmwareVariant_[24] = {};   ///< the build variant, persisted for MoonBase to read

    // Dynamic (updated in tick1s)
    char uptimeStr_[16] = {};
    char fpsStr_[8] = {};
    char tickStr_[8] = {};
    char maxBlockStr_[12] = {};
    uint32_t heapUsedVal_ = 0;
    uint32_t psramUsedVal_ = 0;

    // Static (set in setup)
    uint32_t totalInternalVal_ = 0;
    uint32_t totalHeapVal_ = 0;
    char flashStr_[12] = {};
    uint32_t chipFlashVal_ = 0;     // total chip flash
};

} // namespace mm
