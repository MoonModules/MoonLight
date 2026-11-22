#pragma once

/// What an ESP32 firmware image says about itself, read without ESP-IDF.
///
/// @technical
///
/// ## Why the layout is redefined here
///
/// The layout is a fixed on-disk format the bootloader parses: magic, chip id, then the app descriptor carrying the project name and version, the same bytes on a device and in a test.
///
/// Defining it here rather than including `esp_app_format.h` lets the vetting run in a host test, which matters because the code it guards erases a device's only recovery image.
///
/// `unit_FirmwareImage` pins the layout against real binaries built by the ESP32 toolchain.
///
/// ## The three ways an image can be wrong
///
/// `moonBaseRejection` tests them in the order they are cheapest to detect:
///
/// | Reason | What it usually is |
/// |--------|--------------------|
/// | not an image | a 404 body, an HTML error page, a `.zip` |
/// | the wrong chip | one MoonBase per chip, one paste apart, and a checksum will not catch it |
/// | not MoonBase | an app image, which sits beside it on the releases page |
///
/// ## Two names, because a device refuses a name it does not know
///
/// An image carries its ESP-IDF project name in its descriptor, and every install path checks it before a byte is written, so a build shipping under a new name is refused by everything in the field.
///
/// The name therefore changes in two releases: v6.0.0 knows both and ships under the old one, and the release after it ships under the new one.
///
/// `kAppImageNames` and `kMoonBaseImageNames` hold the pair, first the name a build carries; the old name stays accepted, so a device can go back to v6.0.0 and the app slot keeps refusing an old recovery image.
///
/// One gap stays open, and MIGRATING names it: a device updated over the air from v5 keeps its v5 recovery image, which knows only the old recovery name.
///
/// Offered the new recovery image as an app, that one writes it over the app, so only a cable recovers the device; the gap closes per device once its recovery image is reinstalled.
///
/// ## The handoff version
///
/// The app and MoonBase hand a device to each other through one contract: the install URL staged in NVS under the names below, the config files MoonBase reads its networks, wiring and name from, and the access point both open.
///
/// The contract also holds the HTTP routes the app's pages call on MoonBase: `/moonbase`, `/api/system`, `/api/firmware/upload`, `/api/firmware/url` and `/api/firmware/cancel`.
///
/// `kHandoffVersion` numbers it, as the Linux boot protocol's version numbers the contract between a kernel and its bootloader: a release that leaves the contract alone keeps the number.
///
/// So a MoonBase of an older release, or a local build's newer app, still match; a change to either side raises the number, and the app reports MoonBase behind until the matching one is installed.
///
/// `unit_MoonBaseContract` pins the contract beside the number, read from both images' sources, so a change to it fails until the number is raised with it.
///
/// MoonBase carries its number in a record ESP-IDF places right after the app descriptor (`.rodata_custom_desc`), so the app reads it from the recovery slot as it lies.
///
/// An image lacking the record reads as 0, which matches no contract.
///
/// ## A download that goes silent
///
/// ESP-IDF's OTA read reports a timeout as "still in progress", so a connection that stops delivering bytes without closing keeps an install waiting for good.
///
/// Both install loops, the app's and MoonBase's, therefore abort once no byte has arrived for `kDownloadStallMs`.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm::firmware {

/// First byte of every ESP32 image.
constexpr uint8_t kImageMagic = 0xE9;
/// Marks the app descriptor that follows the image and first segment headers.
constexpr uint32_t kDescMagic = 0xABCD5432;
/// Offset of the app descriptor: a 24-byte image header plus an 8-byte segment header.
constexpr size_t kDescOffset = 32;
/// Offset of the chip id within the image header.
constexpr size_t kChipIdOffset = 12;
/// Enough bytes to identify an image: through the descriptor's version and project name.
constexpr size_t kIdentifyBytes = kDescOffset + 96;

/// Every chip id `esp_chip_id_t` defines, so that no valid image is refused as being for another chip when the truth is that this table does not name it.
enum class ChipId : uint16_t {
    Esp32    = 0x0000,
    Esp32S2  = 0x0002,
    Esp32C3  = 0x0005,
    Esp32S3  = 0x0009,
    Esp32C2  = 0x000C,
    Esp32C6  = 0x000D,
    Esp32H2  = 0x0010,
    Esp32P4  = 0x0012,
    Esp32C5  = 0x0017,
    Esp32H4  = 0x001C,
    Esp32S31 = 0x0020,
    Invalid  = 0xFFFF,
};

/// What an image says about itself. Empty strings when the descriptor is absent or unreadable.
struct ImageInfo {
    bool   valid       = false;   ///< begins with the image magic
    bool   described   = false;   ///< carries a readable app descriptor
    ChipId chip        = ChipId::Invalid;  ///< which chip the image header names
    char   project[32] = {};      ///< one of `kAppImageNames` or `kMoonBaseImageNames`
    char   version[32] = {};      ///< the app version string the descriptor carries
};

/// Read what `buf` claims to be, within `len`, so a truncated or hostile stream reports what it can: @xref{why-the-layout-is-redefined-here}.
inline ImageInfo identify(const uint8_t* buf, size_t len) {
    ImageInfo out;
    if (!buf || len == 0) return out;
    if (buf[0] != kImageMagic) return out;
    out.valid = true;
    if (len >= kChipIdOffset + 2) {
        uint16_t id = 0;
        std::memcpy(&id, buf + kChipIdOffset, 2);
        out.chip = static_cast<ChipId>(id);
    }
    if (len < kDescOffset + 96) return out;
    uint32_t magic = 0;
    std::memcpy(&magic, buf + kDescOffset, 4);
    if (magic != kDescMagic) return out;
    out.described = true;
    // Version at +16 and project name at +48, each a 32-byte field IDF may leave unterminated.
    std::memcpy(out.version, buf + kDescOffset + 16, 31);
    std::memcpy(out.project, buf + kDescOffset + 48, 31);
    return out;
}

/// The names this project's app image carries: @xref{two-names-because-a-device-refuses-a-name-it-does-not-know}.
inline constexpr const char* kAppImageNames[]      = {"MoonLight", "projectMM"};
/// The same pair for the recovery image, which an app slot must never receive.
inline constexpr const char* kMoonBaseImageNames[] = {"MoonLight-moonbase", "projectMM-moonbase"};
/// The migration image (moonbase/migrate), which an app slot takes as an update: it moves the board to the MoonBase layout, a two-slot MoonLight board as a WLED one.
inline constexpr const char* kMigrateImageNames[]  = {"MoonLight-migrate"};

/// The version of the contract between the app and MoonBase: @xref{the-handoff-version}.
inline constexpr uint32_t kHandoffVersion = 1;
/// Where the app stages an install for MoonBase: the NVS namespace, the URL to install, and MoonBase's count of the installs that URL started, which a new URL resets.
inline constexpr const char* kHandoffNvsNamespace = "moonbase";
inline constexpr const char* kHandoffUrlKey       = "url";
inline constexpr const char* kHandoffTriesKey     = "tries";
/// Where the migration image leaves a board's NetworkModule.json, in that namespace, when the new layout's filesystem overlaps the image as it runs; MoonBase writes it into the volume it formats at its first boot.
inline constexpr const char* kMigratedNetworkKey  = "net";
/// Marks the handoff record, so only a record reads as a version, not the bytes after an older image's descriptor: "MMHO" read little-endian.
inline constexpr uint32_t kHandoffMagic = 0x4F484D4D;
/// The record MoonBase carries in `.rodata_custom_desc`.
struct HandoffRecord {
    uint32_t magic;     ///< kHandoffMagic
    uint32_t version;   ///< the kHandoffVersion it was built with
};
/// Offset of the handoff record: the descriptor's own offset plus the 256 bytes of `esp_app_desc_t`.
constexpr size_t kHandoffOffset = kDescOffset + 256;
/// Enough bytes to read the handoff record.
constexpr size_t kHandoffBytes = kHandoffOffset + sizeof(HandoffRecord);

/// The handoff version an image carries, from its first `len` bytes in `buf`; 0 when it carries no record or `buf` stops short of it.
inline uint32_t handoffVersion(const uint8_t* buf, size_t len) {
    if (!buf || len < kHandoffBytes) return 0;
    HandoffRecord rec;
    std::memcpy(&rec, buf + kHandoffOffset, sizeof(rec));
    return rec.magic == kHandoffMagic ? rec.version : 0;
}

/// What an install from a URL says when the device cannot connect to the server, the usual case being an offline device, whose browser can still download the file.
inline constexpr const char* kNoInternetHint = "error: the device cannot reach the internet, download the image and install it from a file";

/// How long an install waits on a download delivering no bytes: @xref{a-download-that-goes-silent}.
inline constexpr uint32_t kDownloadStallMs = 60000;

/// Whether the descriptor field `name`, `cap` bytes that IDF may leave unterminated, is one of `names`.
template <size_t N>
inline bool namedAmong(const char* name, size_t cap, const char* const (&names)[N]) {
    for (const char* known : names) {
        if (std::strncmp(name, known, cap) == 0) return true;
    }
    return false;
}

/// Whether `name` is this project's app image.
inline bool isAppImage(const char* name, size_t cap = 32) { return namedAmong(name, cap, kAppImageNames); }

/// Whether `name` is this project's migration image.
inline bool isMigrateImage(const char* name, size_t cap = 32) { return namedAmong(name, cap, kMigrateImageNames); }

/// Whether an app slot takes `name` as an update: the app, or the migration image, which moves the board to the MoonBase layout from that slot.
inline bool isUpdateImage(const char* name, size_t cap = 32) { return isAppImage(name, cap) || isMigrateImage(name, cap); }

/// Whether `name` is this project's recovery image.
inline bool isMoonBaseImage(const char* name, size_t cap = 32) { return namedAmong(name, cap, kMoonBaseImageNames); }

/// The project name ESP-Hosted's co-processor image carries: the `slave/` project every release builds for the radio chip.
inline constexpr const char* kCoprocessorImageNames[] = {"network_adapter"};

/// Whether `name` is ESP-Hosted's co-processor image.
inline bool isCoprocessorImage(const char* name, size_t cap = 32) { return namedAmong(name, cap, kCoprocessorImageNames); }

/// Whether two `major.minor.patch` versions agree on major and minor, the pair ESP-Hosted requires host and co-processor to share.
inline bool sameMajorMinor(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) return false;
    const char* aDot = std::strchr(a, '.');
    const char* bDot = std::strchr(b, '.');
    if (!aDot || !bDot) return false;
    const char* aEnd = std::strchr(aDot + 1, '.');
    const char* bEnd = std::strchr(bDot + 1, '.');
    const size_t aLen = aEnd ? static_cast<size_t>(aEnd - a) : std::strlen(a);
    const size_t bLen = bEnd ? static_cast<size_t>(bEnd - b) : std::strlen(b);
    return aLen == bLen && std::strncmp(a, b, aLen) == 0;
}

/// The reason `info` is not an image `isKind` accepts for `chip`, or null when it is: the checks in the order they are cheapest to detect.
template <class IsKind>
inline const char* imageRejection(const ImageInfo& info, ChipId chip, IsKind isKind, const char* notKind) {
    if (!info.valid)     return "not a firmware image";
    if (info.chip != chip) return "image is for another chip";
    if (!info.described) return "image carries no description";
    if (!isKind(info.project)) return notKind;
    return nullptr;
}

/// The reason this is not a MoonBase image for `chip`, or null when it is: @xref{the-three-ways-an-image-can-be-wrong}.
inline const char* moonBaseRejection(const ImageInfo& info, ChipId chip) {
    return imageRejection(info, chip, [](const char* p) { return isMoonBaseImage(p); }, "not a MoonBase image");
}

/// The reason this is not a co-processor image for `chip` matching the host's ESP-Hosted `hostVersion`, or null when it is; a mismatched major.minor would leave the link's calls timing out.
inline const char* coprocessorRejection(const ImageInfo& info, ChipId chip, const char* hostVersion) {
    if (const char* why = imageRejection(info, chip, [](const char* p) { return isCoprocessorImage(p); }, "not a radio chip image")) return why;
    if (!sameMajorMinor(info.version, hostVersion)) return "image's ESP-Hosted version differs from this firmware's";
    return nullptr;
}

} // namespace mm::firmware
