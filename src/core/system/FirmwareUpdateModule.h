#pragma once

#include "core/module/MoonModule.h"
#include "core/util/build_info.h"   // kVersion / kRelease / kBuildDate / kFirmwareName
#include "platform/platform.h" // firmwareSize / firmwarePartition

#include <cstdint>
#include <cstdio>
#include <cstring>

/// @defgroup FirmwareUpdateModule Installing a firmware image
/// @{
/// The install's live progress, shared by every path that can start one.
///
/// @moreinfo
///
/// The status and the byte counters are inline globals rather than module state.
/// The flash route and the platform's own task both write them, and both must see one instance.
/// A module reading them reports the same install a socket handler started.
///
/// ## Two addresses, because a rename has to survive in the field
///
/// A device flashed before a rename asks its old repository forever, and GitHub's rename redirect is normally what carries it across.
/// That redirect is the only thing making an in-field update survive the move.
/// This rename vacates a name and re-takes it in one session, which is worth a belt as well as braces.
/// So the update path names both addresses and takes whichever answers.
/// The successor comes first deliberately: once it exists every device reaches it directly, and the redirect stops mattering rather than being depended on forever.
/// Before it exists that request costs one 404, since the predecessor occupying the name publishes no `firmware-*` asset.
/// Fetching another project's firmware is prevented separately.
/// The OTA compares an incoming image's own ESP-IDF descriptor against `kProjectImageName` before a byte is written, so an address answering with a stranger's release is refused rather than flashed.

namespace mm {

/// Where this project's releases will live, tried FIRST so a renamed repository needs no redirect.
constexpr const char* kReleaseRepo = "MoonModules/MoonLight";   // rename-keep: our own future path, which the predecessor occupies until it vacates

/// Where they live today, tried where the address above does not answer.
constexpr const char* kFallbackRepo = "MoonModules/projectMM";   // rename-keep: one repository in both constants leaves a device nowhere to look

/// The release-asset URL a device updates itself from: repository, version, firmware variant, version.
constexpr const char* kReleaseAssetUrlFormat =
    "https://github.com/%s/releases/download/v%s/firmware-%s-v%s.bin";

/// The name this project's app image carries in its ESP-IDF descriptor, which is the CMake `project()` name and changes only with it.
constexpr const char* kProjectImageName = "projectMM";   // a device refuses every firmware if this and CMake disagree, and no build error says so

inline char     g_otaStatus[64]     = "idle";   ///< the phase the install is in, shared by every unit
inline uint32_t g_otaBytesRead      = 0;        ///< how much has been written
inline uint32_t g_otaBytesTotal     = 0;        ///< the image size, zero until it is known

/// Whether an install is running, which both flash paths gate their refusal on.
inline bool otaInFlight() {
    return std::strcmp(g_otaStatus, "starting")    == 0 ||
           std::strcmp(g_otaStatus, "downloading") == 0 ||
           // A prefix, since this one carries its byte counts so the UI can draw a bar.
           std::strncmp(g_otaStatus, "flashing", 8) == 0 ||
           std::strcmp(g_otaStatus, "rebooting")   == 0 ||
           // These matter more: the device has no recovery image during this window.
           std::strcmp(g_otaStatus, "checking")    == 0 ||
           std::strcmp(g_otaStatus, "erasing")     == 0 ||
           // A prefix for the same reason, an exact compare having stopped matching silently.
           std::strncmp(g_otaStatus, "writing MoonBase", 16) == 0;
}

/// The status surface for over-the-air flashing: what is installed, and how an install goes.
///
/// The flash itself is driven by the web route, which hands a URL to the platform task.
/// This module polls that task's progress once a second into its own controls.
/// @card FirmwareUpdateModule.png
///
/// @moreinfo
///
/// ## What the controls describe
///
/// The version is pure semver, so the channel is derivable rather than mixed into it.
/// The build carries the git id first, since that answers which code is on a board.
/// The partition bar shows the image filling its slot, or the incoming one mid-install.
/// Where a device carries two images, a selector says which the rest describe.
///
/// Progress is not a control: it drives the overlay raised during an install.
/// The phase is not one either, surfacing through the shared status slot.
///
/// ## Installing
///
/// The task downloads, writes the next slot, flips the boot pointer, and restarts.
/// The pause before that restart is long enough for the response to reach the browser.
/// Every failure reports through the status slot and stays until the next attempt.
/// A wrong image fails at the start or at boot, and is recoverable over USB.
/// On a device with one app slot the recovery image installs and reboots back.
class FirmwareUpdateModule : public MoonModule {
public:
    /// Keep reporting whatever the toggle says, as the other fixed modules do.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// Prime the buffers from the shared globals, then read the firmware identity.
    void setup() override {
        // So the first state push carries a coherent pair rather than an empty one.
        std::snprintf(statusStr_, sizeof(statusStr_), "%s", g_otaStatus);   // always NUL-terminates
        publishStatus();
        totalSnap_ = g_otaBytesTotal;

        // Pure semver, so the channel stays derivable rather than mixed in.
        std::snprintf(versionStr_, sizeof(versionStr_), "%s", kVersion);
        // The id first, since a timestamp freezes while the firmware moves on.
        std::snprintf(buildStr_, sizeof(buildStr_), "%s · %s", kBuildId, kBuildDate);
        std::snprintf(firmwareStr_, sizeof(firmwareStr_), "%s", kFirmwareName);
        readMoonBaseVersion();
    }

    /// Read which recovery image this device carries, since it drifts and a mismatch matters.
    void readMoonBaseVersion() {
        char installed[32] = {};
        if (platform::otaMoonBaseVersion(installed, sizeof(installed))) {
            const bool matches = std::strcmp(installed, kVersion) == 0;
            std::snprintf(moonbaseStr_, sizeof(moonbaseStr_), "%s%s",
                          installed, matches ? "" : " (outdated)");
        }
        // The rest of its identity, so one row builder serves whichever image is selected.
        platform::otaMoonBaseBuild(moonbaseBuildStr_, sizeof(moonbaseBuildStr_));
    }

    /// Declare one set of controls, describing whichever image the selector names.
    void defineControls() override {
        // Two images described by the same four facts, so eight controls would say each twice.
        if (platform::otaHasMoonBase()) {
            static const char* const kImages[] = { "App", "MoonBase" };
            controls_.addSelect("image", imageSel_, kImages, 2);
            // Drawn as a tab strip rather than a setting among those it governs.
            controls_.setHidden(controls_.count() - 1, true);
        } else {
            imageSel_ = 0;   // nothing else to describe: the app is the only image
        }

        if (imageSel_ == 0) {
            controls_.addReadOnly("version", versionStr_, sizeof(versionStr_));
            controls_.addReadOnly("build", buildStr_, sizeof(buildStr_));
            controls_.addReadOnly("firmware", firmwareStr_, sizeof(firmwareStr_));
            firmwareSizeVal_ = static_cast<uint32_t>(platform::firmwareSize());
            totalFlashVal_ = static_cast<uint32_t>(platform::firmwarePartition());
            if (totalFlashVal_ > 0) {
                controls_.addProgress("partition", firmwareSizeVal_, totalFlashVal_);
            }
        } else {
            // Read here rather than trusting what setup left behind, since this runs per rebuild.
            readMoonBaseVersion();
            platform::otaMoonBaseSize(&moonbaseSizeVal_, &moonbaseTotalVal_);
            controls_.addReadOnly("version", moonbaseStr_, sizeof(moonbaseStr_));
            controls_.addReadOnly("build", moonbaseBuildStr_, sizeof(moonbaseBuildStr_));
            // One image per chip, so the chip is what names its release asset.
            std::snprintf(moonbaseChipStr_, sizeof(moonbaseChipStr_), "%s", platform::chipModel());
            controls_.addReadOnly("firmware", moonbaseChipStr_, sizeof(moonbaseChipStr_));
            if (moonbaseTotalVal_ > 0) {
                controls_.addProgress("partition", moonbaseSizeVal_, moonbaseTotalVal_);
            }
        }

        // The phase rides the shared status slot, and progress the overlay, not a card row.
    }

    /// Rebuild the controls when the selector changes which image they describe.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "image") == 0) rebuildControls();
    }

    /// Poll the install task's progress and phase into the bound buffers.
    void tick1s() MM_NONBLOCKING override {
        // No locks: one writer, and a torn read shows as a brief glimpse.
        std::snprintf(statusStr_, sizeof(statusStr_), "%s", g_otaStatus);   // always NUL-terminates
        publishStatus();
        // A recovery install ends with no reboot, so watching the phase is what notices.
        const bool installing = std::strcmp(statusStr_, "checking") == 0 ||
                                std::strcmp(statusStr_, "erasing") == 0 ||
                                std::strncmp(statusStr_, "writing MoonBase", 16) == 0;
        if (wasInstallingMoonBase_ && !installing) {
            readMoonBaseVersion();
            rebuildControls();
        }
        wasInstallingMoonBase_ = installing;

        // The partition bar doubles as the install bar: the same quantity, measured live.
        const bool writing = otaInFlight();
        if (writing) {
            firmwareSizeVal_ = g_otaBytesRead;
            moonbaseSizeVal_ = g_otaBytesRead;
        } else if (wasWriting_) {
            firmwareSizeVal_ = static_cast<uint32_t>(platform::firmwareSize());
            platform::otaMoonBaseSize(&moonbaseSizeVal_, &moonbaseTotalVal_);
        }
        wasWriting_ = writing;

        // Re-bind when the total lands, since the control captured the old one by value.
        if (g_otaBytesTotal != totalSnap_) {
            totalSnap_ = g_otaBytesTotal;
            rebuildControls();   // re-bind the progress total the overlay reads
        }
    }

    /// Publish the phase on the shared slot, taking its severity from the text's own prefix.
    void publishStatus() {
        if (std::strcmp(statusStr_, "idle") == 0) {
            clearStatus();
        } else {
            setStatus(statusStr_,
                      std::strncmp(statusStr_, "error:", 6) == 0 ? Severity::Error
                                                                 : Severity::Status);
        }
    }

private:
    char     statusStr_[64] = "idle";   ///< the phase, mirrored from the shared global
    uint32_t totalSnap_     = 0;        ///< the total the progress control was bound at
    char     versionStr_[32] = {};   ///< pure semver
    char     buildStr_[48]   = {};   ///< the git id, then the timestamp
    char     firmwareStr_[24] = {};  ///< the build variant's name
    uint32_t firmwareSizeVal_ = 0;   ///< bytes used in the app partition
    uint32_t totalFlashVal_   = 0;   ///< app partition size
    /// The recovery image's version, marked when it differs from this app's.
    char     moonbaseStr_[48] = "standby";
    char     moonbaseBuildStr_[32] = {};   ///< when the installed MoonBase was built
    uint32_t moonbaseSizeVal_  = 0;        ///< bytes its image occupies
    uint32_t moonbaseTotalVal_ = 0;        ///< the factory slot's size
    char     moonbaseChipStr_[16] = {};    ///< the chip whose MoonBase image this board takes
    uint8_t  imageSel_ = 0;                ///< 0 = the app, 1 = MoonBase: which image is described
    bool     wasWriting_ = false;          ///< edge-detects the end of any install, to restore the bar
    bool     wasInstallingMoonBase_ = false;   ///< edge-detects the end of a MoonBase install
};

/// @}
} // namespace mm
