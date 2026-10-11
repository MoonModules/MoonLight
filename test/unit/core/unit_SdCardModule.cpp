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
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

}  // namespace

TEST_CASE("the SD pins are visible, since desktop always reports hasSdCard") {
    SdCardModule sd;
    sd.defineControls();
    for (const char* name : {"clkPin", "cmdPin", "d0Pin", "d1Pin", "d2Pin", "d3Pin", "pwrPin"}) {
        const auto* c = find(sd, name);
        REQUIRE(c != nullptr);
        CHECK(c->hidden == !platform::hasSdCard);
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
