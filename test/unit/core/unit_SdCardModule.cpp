/// @module SdCardModule
/// @also I2cBusModule

/// The SD card is runtime configuration, like AudioService's mic pins.
/// `hasSdCard` is forced true on desktop precisely so these tests exercise the real mount/remount/format path against a temp-dir-backed "/sd", not only the no-hardware stub a chip without the host takes.

#include "doctest.h"
#include "core/system/SdCardModule.h"
#include "platform/platform.h"

#include <cstring>
#include <filesystem>
#include <string>

using namespace mm;

namespace {

const ControlDescriptor* find(const SdCardModule& sd, const char* name) {
    for (uint8_t i = 0; i < sd.controls().count(); i++)
        if (std::strcmp(sd.controls()[i].name, name) == 0) return &sd.controls()[i];
    return nullptr;
}

void setPin(SdCardModule& sd, const char* name, int value) {
    const auto* c = find(sd, name);
    REQUIRE(c != nullptr);
    *static_cast<int8_t*>(c->ptr) = static_cast<int8_t>(value);
}

// A card on a fresh temp filesystem root; the Waveshare P4-ETH's own pins, since any valid set does.
void wireRequiredPins(SdCardModule& sd) {
    setPin(sd, "clkPin", 43);
    setPin(sd, "cmdPin", 44);
    setPin(sd, "d0Pin", 39);
}

struct Rig {
    char root[256];
    SdCardModule sd;
    Rig() {
        static unsigned counter = 0;
        std::snprintf(root, sizeof(root), "%s/mm_sd_test_%u",
                      std::filesystem::temp_directory_path().string().c_str(), counter++);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        platform::fsSetRoot(root);
        sd.setTypeName("SdCardModule");
        sd.defineControls();
    }
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Rig() noexcept {
        sd.release();
        platform::fsSetRoot("");
        platform::setTestSdCardPresent(true);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

// Wires the required pins and mounts, for a test whose point starts after that.
void mountRig(SdCardModule& sd) {
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

TEST_CASE("the SD pins are never hidden: a board without hasSdCard simply never boots this module") {
    SdCardModule sd;
    sd.defineControls();
    for (const char* name : {"clkPin", "cmdPin", "d0Pin", "d1Pin", "d2Pin", "d3Pin", "pwrPin"}) {
        const auto* c = find(sd, name);
        REQUIRE(c != nullptr);
        CHECK_FALSE(c->hidden);
    }
}

TEST_CASE("SdCardModule is a Service") {
    SdCardModule sd;
    CHECK(sd.role() == ModuleRole::Service);
}

TEST_CASE("every SD pin affects prepare, so an edit remounts the card; format does not") {
    SdCardModule sd;
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
    CHECK(find(r.sd, "card (KiB)") == nullptr);
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()) == "card removed");
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
    CHECK(find(r.sd, "card (KiB)") == nullptr);
    REQUIRE(r.sd.status() != nullptr);
    CHECK(std::string(r.sd.status()) == "card removed");
}

TEST_CASE("a reinserted card is noticed within a few seconds: tick1s auto-remounts with no pin touch") {
    Rig r;
    mountRig(r.sd);
    platform::setTestSdCardPresent(false);
    r.sd.tick1s();
    REQUIRE(find(r.sd, "card (KiB)") == nullptr);   // auto-unmounted, as above

    platform::setTestSdCardPresent(true);   // simulate reinsertion
    for (int i = 0; i < 4; i++) {
        r.sd.tick1s();
        CHECK(find(r.sd, "card (KiB)") == nullptr);   // the retry is paced slower than every tick
    }
    r.sd.tick1s();   // the 5th tick since going unmounted: the retry interval elapses
    CHECK(find(r.sd, "card (KiB)") != nullptr);
    CHECK(r.sd.status() == nullptr);
}
