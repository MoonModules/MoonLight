/// @module SdCardService
/// @also I2cBusModule

/// `hasSdCard` is true on the desktop, so these tests run the real mount, remount and format against a temp-dir `/sd`, the card being runtime configuration like AudioService's mic pins.

#include "doctest.h"
#include "core/services/SdCardService.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>

using namespace mm;

namespace {

const ControlDescriptor* find(const SdCardService& sd, const char* name) {
    for (uint8_t i = 0; i < sd.controls().count(); i++)
        if (std::strcmp(sd.controls()[i].name, name) == 0) return &sd.controls()[i];
    return nullptr;
}

void setPin(SdCardService& sd, const char* name, int value) {
    const auto* c = find(sd, name);
    REQUIRE(c != nullptr);
    *static_cast<int8_t*>(c->ptr) = static_cast<int8_t>(value);
}

// A card on a fresh temp filesystem root; the Waveshare P4-ETH's own pins, since any valid set does.
void wireRequiredPins(SdCardService& sd) {
    setPin(sd, "clkPin", 43);
    setPin(sd, "cmdPin", 44);
    setPin(sd, "d0Pin", 39);
}

struct Rig {
    char root[256];
    SdCardService sd;
    Rig() {
        static unsigned counter = 0;
        std::snprintf(root, sizeof(root), "%s/mm_sd_test_%u",
                      std::filesystem::temp_directory_path().string().c_str(), counter++);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        platform::fsSetRoot(root);
        sd.setTypeName("SdCardService");
        sd.defineControls();
    }
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Rig() noexcept {
        platform::setTestSdFormatHeld(false);
        sd.release();
        platform::fsSetRoot("");
        platform::setTestSdCardPresent(true);
        platform::setTestSdCardHasFilesystem(true);
        platform::setTestSdCardReadable(true);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

// The label of the selected board preset.
std::string boardOf(const SdCardService& sd) {
    const auto* c = find(sd, "board");
    REQUIRE(c != nullptr);
    const auto* opts = reinterpret_cast<const char* const*>(c->aux);
    return opts[*static_cast<const uint8_t*>(c->ptr)];
}

// Selects the board preset by label and rebuilds, as the scheduler does on a select change.
void selectBoard(SdCardService& sd, const char* label) {
    const auto* c = find(sd, "board");
    REQUIRE(c != nullptr);
    const auto* opts = reinterpret_cast<const char* const*>(c->aux);
    for (uint8_t k = 0; k < c->max; k++)
        if (std::strcmp(opts[k], label) == 0) *static_cast<uint8_t*>(c->ptr) = k;
    sd.rebuildControls();
}

// Whether a pull was noticed: the gauge gone and the status naming the removal.
bool unmountedAfterPull(const SdCardService& sd) {
    return find(sd, "card (KiB)") == nullptr && sd.status() && std::string(sd.status()).rfind("card removed", 0) == 0;
}

// Wires the required pins and mounts, for a test whose point starts after that.
void mountRig(SdCardService& sd) {
    wireRequiredPins(sd);
    sd.prepare();
    REQUIRE(find(sd, "card (KiB)") != nullptr);
}

// Writes "hello" to /sd/clip.bin and reopens it for reading, so a test can hold the busy guard open without repeating the round trip.
platform::FsReadStream openClipForReading() {
    platform::FsWriteStream w{};
    REQUIRE(platform::fsWriteStreamOpen(w, "/sd/clip.bin"));
    REQUIRE(platform::fsWriteStreamWrite(w, "hello", 5));
    REQUIRE(platform::fsWriteStreamClose(w, true));
    platform::FsReadStream rs{};
    REQUIRE(platform::fsReadStreamOpen(rs, "/sd/clip.bin"));
    return rs;
}

}  // namespace

TEST_CASE("the pins show under Custom, the default, and hide under a board preset that sets them") {
    SdCardService sd;
    sd.rebuildControls();
    CHECK(boardOf(sd) == "Custom");
    for (const char* name : {"clkPin", "cmdPin", "d0Pin", "d1Pin", "d2Pin", "d3Pin", "pwrPin"}) CHECK_FALSE(find(sd, name)->hidden);

    selectBoard(sd, "P4 SDMMC");
    for (const char* name : {"clkPin", "cmdPin", "d0Pin", "d1Pin", "d2Pin", "d3Pin", "pwrPin"}) CHECK(find(sd, name)->hidden);
}

TEST_CASE("a board preset writes its wiring and the mount follows; Custom keeps it to edit") {
    Rig r;
    selectBoard(r.sd, "P4 SDMMC");
    CHECK(*static_cast<int8_t*>(find(r.sd, "clkPin")->ptr) == 43);
    CHECK(*static_cast<int8_t*>(find(r.sd, "d3Pin")->ptr) == 42);
    CHECK(*static_cast<int8_t*>(find(r.sd, "pwrPin")->ptr) == 45);
    CHECK(r.sd.affectsPrepare("board"));
    r.sd.prepare();
    CHECK(find(r.sd, "card (KiB)") != nullptr);

    selectBoard(r.sd, "Custom");
    CHECK_FALSE(find(r.sd, "clkPin")->hidden);
    CHECK(*static_cast<int8_t*>(find(r.sd, "clkPin")->ptr) == 43);   // Custom writes nothing, so the wiring stays to edit
}

// What a restore does with a config that holds a board's pins and no saved `board`: the card opens on that board's name.
TEST_CASE("pins that match a board preset open on its name rather than on Custom") {
    SdCardService sd;
    sd.rebuildControls();
    for (const auto& [name, value] : {std::pair<const char*, int>{"clkPin", 43}, {"cmdPin", 44}, {"d0Pin", 39},
                                      {"d1Pin", 40}, {"d2Pin", 41}, {"d3Pin", 42}, {"pwrPin", 45}}) setPin(sd, name, value);
    sd.rebuildControls();
    CHECK(boardOf(sd) == "P4 SDMMC");
    sd.rebuildControls();   // and the seed keeps a chosen preset on the next rebuild
    CHECK(boardOf(sd) == "P4 SDMMC");
}

// Asked per chip rather than per build, since a host test runs as the desktop, which previews every preset.
namespace {
using Chip = SdCardService::SdPresetChip;
constexpr SdCardService::SdChip kClassic{false, true, false}, kP4{false, false, true}, kS3{false, false, false}, kDesktop{true, true, false};
}  // namespace

TEST_CASE("each chip offers only the SD board presets it can wire") {
    for (const auto& chip : {kClassic, kP4, kS3, kDesktop}) CHECK(SdCardService::presetFits(Chip::Any, chip));
    CHECK(SdCardService::presetFits(Chip::P4, kP4));
    CHECK_FALSE(SdCardService::presetFits(Chip::P4, kClassic));
    CHECK_FALSE(SdCardService::presetFits(Chip::P4, kS3));        // an S3 or S31 has no board preset
    CHECK(SdCardService::presetFits(Chip::Classic, kClassic));
    CHECK_FALSE(SdCardService::presetFits(Chip::Classic, kP4));
    CHECK_FALSE(SdCardService::presetFits(Chip::Classic, kS3));
    for (Chip c : {Chip::Classic, Chip::P4}) CHECK(SdCardService::presetFits(c, kDesktop));   // the desktop previews them all
}

// Every P4 board with a slot wires the same pins, so a P4 opens on that preset; a classic has two to choose from, an S3 none, and the desktop previews them all.
TEST_CASE("a P4 opens on its one SD board preset, every other build on Custom") {
    CHECK(SdCardService::opensOnSolePreset(kP4));
    CHECK_FALSE(SdCardService::opensOnSolePreset(kClassic));
    CHECK_FALSE(SdCardService::opensOnSolePreset(kS3));
    CHECK_FALSE(SdCardService::opensOnSolePreset(kDesktop));
}

TEST_CASE("an SPI preset sets the bus, warns about long writes once mounted, and the mount needs its CS") {
    Rig r;
    selectBoard(r.sd, "Classic SPI");
    CHECK(*static_cast<uint8_t*>(find(r.sd, "bus")->ptr) == static_cast<uint8_t>(platform::SdBus::Spi));
    CHECK(*static_cast<int8_t*>(find(r.sd, "d0Pin")->ptr) == 36);   // MISO
    CHECK(*static_cast<int8_t*>(find(r.sd, "d3Pin")->ptr) == 16);   // CS
    r.sd.prepare();
    CHECK(find(r.sd, "card (KiB)") != nullptr);
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()).rfind("SPI: writes over 50 KB", 0) == 0);

    selectBoard(r.sd, "Custom");
    CHECK_FALSE(find(r.sd, "bus")->hidden);
    CHECK(find(r.sd, "d1Pin")->hidden);
    CHECK(find(r.sd, "d2Pin")->hidden);
    CHECK_FALSE(find(r.sd, "d3Pin")->hidden);

    setPin(r.sd, "d3Pin", -1);   // SPI with no CS cannot select the card
    r.sd.prepare();
    CHECK(find(r.sd, "card (KiB)") == nullptr);
}

TEST_CASE("SdCardService is a Service") {
    SdCardService sd;
    CHECK(sd.role() == ModuleRole::Service);
}

TEST_CASE("every SD pin affects prepare, so an edit remounts the card; format does not") {
    SdCardService sd;
    for (const char* name : {"clkPin", "cmdPin", "d0Pin", "d1Pin", "d2Pin", "d3Pin", "pwrPin"})
        CHECK(sd.affectsPrepare(name));
    CHECK_FALSE(sd.affectsPrepare("format"));
}

TEST_CASE("prepare refuses to mount with no pins set") {
    Rig r;
    r.sd.prepare();
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()) == "set the SD card pins first");
    CHECK(find(r.sd, "card (KiB)") == nullptr);
}

TEST_CASE("prepare mounts once the required pins are set, and the gauge appears") {
    Rig r;
    wireRequiredPins(r.sd);
    r.sd.prepare();
    CHECK(r.sd.status() == nullptr);
    CHECK(std::filesystem::exists(std::string(r.root) + "/sd"));
    CHECK(find(r.sd, "card (KiB)") != nullptr);
}

TEST_CASE("a pin edit remounts the card live, the same contract I2cBusModule's sda/scl have") {
    Rig r;
    wireRequiredPins(r.sd);
    r.sd.prepare();
    REQUIRE(find(r.sd, "card (KiB)") != nullptr);

    // Unset a required pin: prepare() must unmount on the next pass, same as the live-reconfig contract promises.
    setPin(r.sd, "d0Pin", -1);
    r.sd.prepare();
    CHECK(find(r.sd, "card (KiB)") == nullptr);
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()) == "set the SD card pins first");
}

TEST_CASE("release unmounts; it is harmless when nothing was mounted") {
    Rig r;
    r.sd.release();   // nothing mounted yet: must not crash or misbehave
    CHECK(find(r.sd, "card (KiB)") == nullptr);

    wireRequiredPins(r.sd);
    r.sd.prepare();
    REQUIRE(find(r.sd, "card (KiB)") != nullptr);
    r.sd.release();
    // The card still exists on disk; this instance only let go of it.
    CHECK(std::filesystem::exists(std::string(r.root) + "/sd"));
}

TEST_CASE("format refuses while unmounted, and wipes the card while mounted") {
    Rig r;
    r.sd.onControlChanged("format");
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()) == "mount a card before formatting");

    wireRequiredPins(r.sd);
    r.sd.prepare();
    std::filesystem::create_directories(std::string(r.root) + "/sd/leftover");
    REQUIRE(std::filesystem::exists(std::string(r.root) + "/sd/leftover"));

    r.sd.onControlChanged("format");
    CHECK(r.sd.status() == nullptr);
    CHECK_FALSE(std::filesystem::exists(std::string(r.root) + "/sd/leftover"));   // wiped
    CHECK(std::filesystem::exists(std::string(r.root) + "/sd"));                  // recreated, still mounted
}

TEST_CASE("an unrelated prepare does not remount when the pins have not changed") {
    Rig r;
    mountRig(r.sd);

    platform::FsWriteStream w{};
    REQUIRE(platform::fsWriteStreamOpen(w, "/sd/clip.bin"));

    r.sd.prepare();   // same pins: prepare() runs tree-wide on any affectsPrepare change, so this must be a no-op
    CHECK(r.sd.status() == nullptr);               // never tried to unmount, so no "busy" status either
    CHECK(find(r.sd, "card (KiB)") != nullptr);     // mount state undisturbed

    REQUIRE(platform::fsWriteStreamWrite(w, "hello", 5));   // the stream the no-op prepare() did not touch
    REQUIRE(platform::fsWriteStreamClose(w, true));
    CHECK(std::filesystem::exists(std::string(r.root) + "/sd/clip.bin"));
}

TEST_CASE("format refuses while a download stream is open, and succeeds once it closes") {
    Rig r;
    mountRig(r.sd);

    platform::FsReadStream rs = openClipForReading();

    r.sd.onControlChanged("format");   // a download is in flight
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()) == "busy: transfer in progress");
    CHECK(std::filesystem::exists(std::string(r.root) + "/sd/clip.bin"));   // not wiped

    platform::fsReadStreamClose(rs);
    r.sd.onControlChanged("format");   // the stream is gone: format now runs
    CHECK(r.sd.status() == nullptr);
    CHECK_FALSE(std::filesystem::exists(std::string(r.root) + "/sd/clip.bin"));
}

TEST_CASE("tick1s is a no-op while unmounted, and refreshes the gauge once mounted") {
    Rig r;
    for (int i = 0; i < 120; i++) r.sd.tick1s();   // two minutes' worth, nothing to show
    CHECK(find(r.sd, "card (KiB)") == nullptr);

    wireRequiredPins(r.sd);
    r.sd.prepare();
    REQUIRE(find(r.sd, "card (KiB)") != nullptr);
    for (int i = 0; i < 60; i++) r.sd.tick1s();   // the minute-paced scan fires once
    CHECK(find(r.sd, "card (KiB)") != nullptr);
}

TEST_CASE("a pulled card is noticed within a second: tick1s auto-unmounts with no pin touch") {
    Rig r;
    mountRig(r.sd);

    platform::setTestSdCardPresent(false);   // simulate the card being pulled
    r.sd.tick1s();
    CHECK(unmountedAfterPull(r.sd));
}

TEST_CASE("a pull mid-transfer waits for the busy guard, same as a manual unmount would") {
    Rig r;
    mountRig(r.sd);

    platform::FsReadStream rs = openClipForReading();   // an open stream holds the busy guard

    platform::setTestSdCardPresent(false);
    r.sd.tick1s();
    // Refused underneath the open stream: still mounted, no "card removed" status yet.
    CHECK(find(r.sd, "card (KiB)") != nullptr);
    CHECK(r.sd.status() == nullptr);

    platform::fsReadStreamClose(rs);
    r.sd.tick1s();   // the next poll succeeds now that nothing holds the guard
    CHECK(unmountedAfterPull(r.sd));
}

// Mounting an empty slot stalls the render thread, so no timer and no unrelated prepare retries it; switching the module off and on tries again.
TEST_CASE("a card inserted after a failed mount mounts when the module is switched off and on, never on a timer") {
    Rig r;
    platform::setTestSdCardPresent(false);   // an empty slot
    wireRequiredPins(r.sd);
    r.sd.prepare();
    REQUIRE(find(r.sd, "card (KiB)") == nullptr);
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()).rfind("no card answered", 0) == 0);

    platform::setTestSdCardPresent(true);    // inserted later
    r.sd.prepare();                          // an unrelated prepare, same pins: the slot is not tried again
    for (int i = 0; i < 120; i++) r.sd.tick1s();   // nor on a timer
    CHECK(find(r.sd, "card (KiB)") == nullptr);

    r.sd.release();                          // switched off
    r.sd.prepare();                          // and on
    CHECK(find(r.sd, "card (KiB)") != nullptr);
    CHECK(r.sd.status() == nullptr);
}

// A card that answers but holds no FAT filesystem (fresh, or used in a camera) says so, and format writes one and mounts it; an empty slot stays refused.
TEST_CASE("a card without a FAT filesystem says so, and format makes one and mounts it") {
    Rig r;
    platform::setTestSdCardHasFilesystem(false);
    wireRequiredPins(r.sd);
    r.sd.prepare();
    REQUIRE(find(r.sd, "card (KiB)") == nullptr);
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()).rfind("the card has no FAT filesystem", 0) == 0);

    r.sd.onControlChanged("format");
    CHECK(find(r.sd, "card (KiB)") != nullptr);   // formatted and mounted in one press
    CHECK(r.sd.status() == nullptr);
}

TEST_CASE("format refuses on an empty slot: only a card without a filesystem is formatted unmounted") {
    Rig r;
    platform::setTestSdCardPresent(false);
    wireRequiredPins(r.sd);
    r.sd.prepare();
    r.sd.onControlChanged("format");
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()) == "mount a card before formatting");
    CHECK(find(r.sd, "card (KiB)") == nullptr);
}

TEST_CASE("a card that answers but does not read says so, and format does not offer to fix it") {
    Rig r;
    platform::setTestSdCardReadable(false);
    wireRequiredPins(r.sd);
    r.sd.prepare();
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()).rfind("the card answers but does not read", 0) == 0);

    r.sd.onControlChanged("format");
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()) == "mount a card before formatting");
}

// A large card's format outlasts the watchdog, so it runs in the background and the module waits it out a tick at a time.
TEST_CASE("a format runs in the background: the card is busy and the module says formatting until it finishes") {
    Rig r;
    mountRig(r.sd);
    platform::setTestSdFormatHeld(true);

    r.sd.onControlChanged("format");
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()).rfind("formatting", 0) == 0);
    CHECK(find(r.sd, "card (KiB)") == nullptr);   // unmounted while it formats
    CHECK(platform::sdCardBusy());
    CHECK_FALSE(platform::sdUnmount());

    setPin(r.sd, "d0Pin", 40);   // a pin edit mid-format waits too
    r.sd.prepare();
    r.sd.onControlChanged("format");   // and so does a second press
    for (int i = 0; i < 30; i++) r.sd.tick1s();
    CHECK(std::string(r.sd.status()).rfind("formatting", 0) == 0);

    platform::setTestSdFormatHeld(false);   // the format finishes
    r.sd.tick1s();
    CHECK(r.sd.status() == nullptr);
    CHECK(find(r.sd, "card (KiB)") != nullptr);
    CHECK_FALSE(platform::sdCardBusy());
}

// A card that stops answering mid-write cannot delete its temporary file, so the next mount empties the staging folder.
TEST_CASE("a mount clears the temporary files a failed write left in the staging folder") {
    Rig r;
    std::filesystem::create_directories(std::string(r.root) + "/sd/.tmp");
    { std::FILE* f = std::fopen((std::string(r.root) + "/sd/.tmp/w7.tmp").c_str(), "wb"); REQUIRE(f); std::fclose(f); }
    mountRig(r.sd);
    CHECK_FALSE(std::filesystem::exists(std::string(r.root) + "/sd/.tmp/w7.tmp"));
}
