#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
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
/// A device flashed before v5.0.0 asks the repository's earlier name forever, and GitHub's rename redirect is what carries it across.
/// The update path names both addresses and takes whichever answers, so an in-field update rests on more than that redirect.
/// The current name comes first: every device reaches it directly, and the earlier one is a second chance when that request fails.
/// Fetching another project's firmware is prevented separately.
/// The OTA compares an incoming image's own ESP-IDF descriptor against the names in `FirmwareImage.h` before a byte is written.
/// An address answering with a stranger's release is refused rather than flashed.

namespace mm {

/// Where this project's releases live, tried FIRST so a device needs no redirect.
constexpr const char* kReleaseRepo = "MoonModules/MoonLight";

/// The repository's earlier name, which GitHub redirects, tried where the address above does not answer: one repository in both constants leaves a device nowhere to look.
constexpr const char* kFallbackRepo = "MoonModules/projectMM";

/// The release-asset URL a device updates itself from: repository, version, firmware variant, version.
constexpr const char* kReleaseAssetUrlFormat =
    "https://github.com/%s/releases/download/v%s/firmware-%s-v%s.bin";

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
        if (moonbaseOutdated_) rebuildControls();   // publish the mismatch the first definition could not know
    }

    /// Read which recovery image this device carries, since it drifts and a mismatch matters.
    void readMoonBaseVersion() {
        char installed[32] = {};
        moonbaseOutdated_ = false;
        if (platform::otaMoonBaseVersion(installed, sizeof(installed))) {
            // `6.0.0+990a3d84`: SemVer's build metadata carries the build id, as the app's own build row shows it.
            char* id = std::strchr(installed, '+');
            if (id) *id++ = '\0';
            std::snprintf(moonbaseStr_, sizeof(moonbaseStr_), "%s", installed);
            char built[32] = {};
            platform::otaMoonBaseBuild(built, sizeof(built));
            std::snprintf(moonbaseBuildStr_, sizeof(moonbaseBuildStr_), "%s%s%s",
                          id ? id : "", id && built[0] ? " · " : "", built);
            // The matching pair is the tested pair, whichever channel the app follows.
            moonbaseOutdated_ = std::strcmp(moonbaseStr_, kVersion) != 0;
        }
    }

    /// Declare one set of controls, describing whichever image the selector names.
    void defineControls() override {
        // On either tab, for the update signals: the app's version, and MoonBase's only while it does not match, so this module is the one place that decides.
        controls_.addReadOnly("appVersion", versionStr_, sizeof(versionStr_));
        controls_.setHidden(controls_.count() - 1, true);
        if (moonbaseOutdated_) {
            controls_.addReadOnly("moonbaseMismatch", moonbaseStr_, sizeof(moonbaseStr_));
            controls_.setHidden(controls_.count() - 1, true);
        }
        if (platform::otaHasMoonBase()) {
            // Two images described by the same four facts, so eight controls would say each twice.
            static const char* const kImages[] = { "App", "MoonBase" };
            controls_.addSelect("image", imageSel_, kImages, kImageCount);
            // Drawn as a tab strip rather than a setting among those it governs.
            controls_.setHidden(controls_.count() - 1, true);
        } else {
            imageSel_ = kImageApp;   // nothing else to describe: the app is the only image
        }

        if (imageSel_ == kImageApp) {
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
        mm::formatTo(statusStr_, sizeof(statusStr_), "%s", g_otaStatus);   // always NUL-terminates
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

    /// Publish the phase on the shared slot, taking its severity from the text's own prefix; between installs, a MoonBase that does not match this app.
    void publishStatus() {
        if (std::strcmp(statusStr_, "idle") == 0) {
            // A constant, since both versions are on the card's rows already.
            if (moonbaseOutdated_) setStatus("MoonBase does not match this app: install the matching one on the MoonBase tab", Severity::Warning);
            else clearStatus();
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
    char     moonbaseStr_[32] = "standby";   ///< the recovery image's version
    char     moonbaseBuildStr_[36] = {};   ///< its build id, then when it was built: `abc12345+ · Oct  4 2026 10:31:00` is 34
    bool     moonbaseOutdated_ = false;    ///< it differs from this app's version
    uint32_t moonbaseSizeVal_  = 0;        ///< bytes its image occupies
    uint32_t moonbaseTotalVal_ = 0;        ///< the factory slot's size
    char     moonbaseChipStr_[16] = {};    ///< the chip whose MoonBase image this board takes
    /// The images `imageSel_` picks from, in kImages order.
    enum Image : uint8_t { kImageApp, kImageMoonBase, kImageCount };
    uint8_t  imageSel_ = kImageApp;        ///< which image is described
    bool     wasWriting_ = false;          ///< edge-detects the end of any install, to restore the bar
    bool     wasInstallingMoonBase_ = false;   ///< edge-detects the end of a MoonBase install
};

/// @}
} // namespace mm
