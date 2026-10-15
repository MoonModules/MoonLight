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

/// The device's identity and vitals: its name, how long it has run, and the memory and tick it renders at.
/// @card SystemModule.png
///
/// The name is the one the network sees, behind mDNS `<name>.local`, the access point SSID and the DHCP hostname. The readings are in @xref{what-it-reports|More info}.
///
/// @guide
///
/// ## Safe mode
///
/// Two failed boots in a row start the next boot with the LEDs off, no MoonLive script and at most 1,024 lights. A failed boot is a crash, a brownout, or a power loss within a minute of starting. Every boot raises the lights over five seconds.
///
/// @moreinfo
///
/// ## What it reports
///
/// The dynamic readings refresh every second: uptime, frames a second, tick time, heap and PSRAM. The static ones are read once at boot: chip, CPU, SDK, flash and the reset reason.
///
/// PSRAM is derived, since the allocator merges its pool at boot: a total above the internal total is the signal. The co-processor readout appears only where the radio is a separate chip.
///
/// ## The two identities
///
/// `deviceName` is the one network identity, behind mDNS, the SoftAP SSID and DHCP. It is coerced to a valid hostname every tick, so a rename propagates within one tick.
///
/// `deviceModel` is the physical board from the device-model catalog, pushed by tooling since a device cannot identify its own hardware; a validator on the control checks every write path.
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

        if (safeMode()) {
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "Safe mode after %u failed boots in a row (%s): lights dark and scripts off; restart to run normally",
                         static_cast<unsigned>(failedBoots()), platform::resetReason());
            setStatus(statusBuf_, Severity::Warning);
        }

        MoonModule::setup();
    }

    /// The persisted log level, which the main loop reads to gate its once-a-second line.
    platform::LogLevel logLevel() const { return static_cast<platform::LogLevel>(logLevel_); }

    /// Apply a log-level change live, and re-assert the firmware variant a load overwrote.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "logLevel") == 0) applyLogLevel();
        // No flush first, since a save would write back what the reset deletes.
        if (std::strcmp(controlName, "factory reset") == 0) {
            if (factoryReset()) platform::reboot();
            else setStatus("factory reset failed: some settings could not be deleted", Severity::Warning);
        }
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

        // Device name on top: @xref{the-two-identities}.
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
            if (const char* pt = platform::psramType(); pt && pt[0]) {
                /// The PSRAM interface and clock.
                controls_.addReadOnly("psramType", const_cast<char*>(pt));
                controls_.setAdvanced(controls_.count() - 1);   // for whoever asks why a board is slow, not for running a show
            }
        }
        controls_.addReadOnly("maxBlock", maxBlockStr_, sizeof(maxBlockStr_));

        // Firmware identity lives on the firmware card, and filesystem usage on the file manager.
        if (chipFlashVal_ > 0) {
            controls_.addReadOnly("flash", flashStr_, sizeof(flashStr_));
            // Read once from the hardware: a board updated over the network keeps its bootloader's mode, and DIO fetches code over two lines where QIO uses four.
            if (const char* fm = platform::flashMode(); fm && fm[0]) {
                /// How the chip reads its flash, such as QIO or DIO.
                controls_.addReadOnly("flashMode", const_cast<char*>(fm));
                controls_.setAdvanced(controls_.count() - 1);   // expert, as psramType
            }
        }

        // These never change at runtime, so each binds straight to the platform's own buffer.
        /// The hardware address, which tooling identifies the device by.
        controls_.addReadOnly("mac", const_cast<char*>(platform::macString()));
        /// The chip model.
        controls_.addReadOnly("chip", const_cast<char*>(platform::chipModel()));
        /// The clock and core count.
        controls_.addReadOnly("cpu", const_cast<char*>(platform::cpuInfo()));
        /// The SDK version the firmware is built on.
        controls_.addReadOnly("sdk", const_cast<char*>(platform::sdkVersion()));
        /// Why the device last restarted.
        controls_.addReadOnly("bootReason", const_cast<char*>(platform::resetReason()));
        // The UI honors this client-side, so nothing in the firmware reads it. A LEVEL rather than a switch, because a control names the audience it is for and one number decides which audience is reading.
        controls_.addSelect("mode", mode_, kModeOptions, 3);
        /// Delete every setting and preset and restart as freshly installed.
        /// @guide Press twice to confirm. Scripts and other files stay.
        controls_.addButton("factory reset", /*confirm*/ true);
        controls_.addSelect("logLevel", logLevel_, logLevelOptions_, 6);
        controls_.setDeveloper(controls_.count() - 1);
        // Compiled out where the radio is native, so the control and its query both vanish.
        if constexpr (platform::hasWifiCoprocessor) {
            /// The firmware version of the separate WiFi chip.
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

    /// Delete every setting and preset, so the next boot starts as freshly installed; scripts and other files stay.
    static bool factoryReset() { return FilesystemModule::removeTree(FilesystemModule::CONFIG_DIR); }

    /// Accept printable ASCII within the buffer, which every write path checks; empty is the unset model a board starts with.
    static bool validateDeviceModel(const char* value) {
        if (!value) return false;
        size_t n = std::strlen(value);
        if (n >= 32) return false;   // 0..31 (32-byte buffer, NUL-terminated)
        for (size_t i = 0; i < n; i++) {
            unsigned char b = static_cast<unsigned char>(value[i]);
            if (b < 0x20 || b > 0x7E) return false;
        }
        return true;
    }

private:
    Scheduler* scheduler_ = nullptr;
    char statusBuf_[128] = {};   ///< the safe-mode line, 120 with a 9-character reset reason

    /// The name behind mDNS, the access point and DHCP.
    /// @guide Anything but letters, digits and hyphens is coerced to a valid hostname, and a rename reaches the network within a tick.
    char deviceName_[24] = {};
    /// How much of the UI is shown, cumulative.
    /// @guide User shows the essentials, expert adds the 🎚️ controls, developer the 🔧 ones. Only the UI reads it.
    uint8_t mode_ = 0;
    static constexpr const char* kModeOptions[3] = {"user", "expert", "developer"};
    /// Push the level to the logger, clamped so a corrupt value cannot index past the end.
    void applyLogLevel() {
        uint8_t lvl = logLevel_ > static_cast<uint8_t>(platform::LogLevel::Verbose)
                          ? static_cast<uint8_t>(platform::LogLevel::Verbose) : logLevel_;
        platform::setLogLevel(static_cast<platform::LogLevel>(lvl));
    }

    /// Serial log verbosity.
    /// @guide Warn by default. The first 60 seconds always log at Info, which is when the installer reads the IP.
    uint8_t logLevel_ = static_cast<uint8_t>(platform::LogLevel::Warn);
    static constexpr const char* logLevelOptions_[] = {"None", "Error", "Warn", "Info", "Debug", "Verbose"};
    /// The board model, which selects the installer's catalog entry.
    /// @guide Tooling sets it, since a device cannot identify its own hardware.
    char deviceModel_[32] = {};
    /// The build variant, kept for the MoonBase recovery image to read.
    char     firmwareVariant_[24] = {};

    // Dynamic (updated in tick1s)
    /// Time since boot.
    char uptimeStr_[16] = {};
    /// Frames rendered per second.
    char fpsStr_[8] = {};
    /// Microseconds one tick takes.
    char tickStr_[8] = {};
    /// The largest free internal block, which bounds one allocation.
    char maxBlockStr_[12] = {};
    /// Internal heap in use.
    uint32_t heapUsedVal_ = 0;
    /// PSRAM in use.
    uint32_t psramUsedVal_ = 0;

    // Static (set in setup)
    uint32_t totalInternalVal_ = 0;
    uint32_t totalHeapVal_ = 0;
    /// The flash chip size.
    char flashStr_[12] = {};
    uint32_t chipFlashVal_ = 0;     // total chip flash
};

} // namespace mm
