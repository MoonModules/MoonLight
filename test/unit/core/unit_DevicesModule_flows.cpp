/// @module DevicesModule
/// @also NetworkUse
///
/// Pins how Discovery turns what the modules report of their network use into the map: a row per peer, edges as each row's adjacency list, services on this device's row.
///
/// @technical
///
/// ## How the test reaches it
///
/// A stand-in module reports fixed services and flows; Discovery walks the tree once a second in tick1s, which runs once the device has an address, so the test gives it one.

#include "doctest.h"
#include "discovery_rows.h"
#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/system/DevicesModule.h"
#include "core/util/JsonSink.h"
#include "fake_light_output.h"   // a light output, whose drivers this device's row names
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace mm;

namespace {

// Restores the host's own address and the real clock, even when an assertion throws.
struct Guard {
    ~Guard() { platform::setTestEthIPv4(nullptr); platform::setTestNowMs(0); }
};

// Stands in for a sender and a receiver: one service, a stream out to a board, one to a group, and one in from another board.
struct Reporter : MoonModule {
    void reportNetworkUse(NetworkUse& use) const override {
        static constexpr uint8_t board[4] = {192, 0, 2, 50}, group[4] = {239, 0, 0, 1}, sender[4] = {192, 0, 2, 60};
        use.service("DDP", 4048);
        use.flow("DDP", true, board, 43, "/s");
        use.flow("audio sync", true, group, 40, "/s");
        use.flow("Art-Net", false, sender, 25, "/s");
    }
};

// The detail of the row at `ip`, the map's view of it, or "" when there is none.
std::string detailFor(const DevicesModule& dev, const char* ip) { return discovery_rows::rowAt(dev, ip, true); }

// A tree of the reporter and Discovery, ticked once with this device at 192.0.2.1.
struct Rig {
    Guard guard;
    Scheduler scheduler;
    Reporter* reporter = new Reporter();
    DevicesModule* dev = new DevicesModule();
    Rig() {
        const uint8_t self[4] = {192, 0, 2, 1};
        platform::setTestEthIPv4(self);
        platform::setTestNowMs(1000);
        reporter->setName("Reporter");
        dev->setName("Discovery");
        scheduler.addModule(reporter);
        scheduler.addModule(dev);
        scheduler.setup();
        dev->tick1s();
    }
    ~Rig() { scheduler.release(); }   // deletes the tree
};

}  // namespace

TEST_CASE("DevicesModule: a reported flow gives its peer a row, a multicast destination a group") {
    Rig rig;
    CHECK(std::strstr(detailFor(*rig.dev, "192.0.2.50").c_str(), "\"type\":\"generic\"") != nullptr);
    CHECK(std::strstr(detailFor(*rig.dev, "192.0.2.60").c_str(), "\"type\":\"generic\"") != nullptr);
    CHECK(std::strstr(detailFor(*rig.dev, "239.0.0.1").c_str(), "\"type\":\"group\"") != nullptr);
}

TEST_CASE("DevicesModule: this device's row carries its outbound flows as edges and its services") {
    Rig rig;
    const std::string self = detailFor(*rig.dev, "192.0.2.1");
    CHECK(std::strstr(self.c_str(), "{\"to\":\"192.0.2.50\",\"label\":\"DDP 43/s\"}") != nullptr);
    CHECK(std::strstr(self.c_str(), "{\"to\":\"239.0.0.1\",\"label\":\"audio sync 40/s\"}") != nullptr);
    CHECK(std::strstr(self.c_str(), "\"services\":[\"DDP 4048\"]") != nullptr);
    CHECK(std::strstr(self.c_str(), "192.0.2.60") == nullptr);   // an inbound flow is drawn from the sender's row
}

TEST_CASE("DevicesModule: an inbound flow is an edge from the sender's row to this device") {
    Rig rig;
    const std::string sender = detailFor(*rig.dev, "192.0.2.60");
    CHECK(std::strstr(sender.c_str(), "{\"to\":\"192.0.2.1\",\"label\":\"Art-Net 25/s\"}") != nullptr);
    const std::string receiver = detailFor(*rig.dev, "192.0.2.50");
    REQUIRE(!receiver.empty());   // the row is there, so a missing edge is its own
    CHECK(std::strstr(receiver.c_str(), "\"edges\"") == nullptr);   // a receiver's row has no edge of its own
}

TEST_CASE("DevicesModule: a disabled module reports nothing") {
    Rig rig;
    rig.reporter->setEnabled(false);
    platform::setTestNowMs(2000);
    rig.dev->tick1s();
    const std::string self = detailFor(*rig.dev, "192.0.2.1");
    REQUIRE(!self.empty());   // this device keeps its row, only without flows or services
    CHECK(std::strstr(self.c_str(), "\"edges\"") == nullptr);
    CHECK(std::strstr(self.c_str(), "\"services\"") == nullptr);
}

TEST_CASE("DevicesModule: this device's row names what drives its lights, while a light output runs") {
    Rig rig;
    CHECK(std::strstr(detailFor(*rig.dev, "192.0.2.1").c_str(), "\"outputs\"") == nullptr);   // no light output, no outputs
    struct Lights final : test::FakeLightOutput {} lights;
    lights.outputs_ = "{\"driver\":\"Hub75Driver\",\"lights\":4096}";
    lights.outputSeat_.claim();
    const std::string self = detailFor(*rig.dev, "192.0.2.1");
    REQUIRE(!self.empty());
    CHECK(std::strstr(self.c_str(), "\"outputs\":[{\"driver\":\"Hub75Driver\",\"lights\":4096}]") != nullptr);
    CHECK(std::strstr(detailFor(*rig.dev, "192.0.2.50").c_str(), "\"outputs\"") == nullptr);   // a peer's row carries its own, from its own list
    lights.outputSeat_.vacate();
}

TEST_CASE("DevicesModule: a peer a module names, such as Home Assistant from its User-Agent, is listed by that name") {
    Rig rig;
    struct Named final : MoonModule {
        void reportNetworkUse(NetworkUse& use) const override {
            const uint8_t ha[4] = {192, 0, 2, 70};
            use.flow("WLED API", false, ha, 6, "/m", "Home Assistant");
        }
    };
    auto* named = new Named();
    named->setName("Named");
    rig.scheduler.addModule(named);
    platform::setTestNowMs(2000);
    rig.dev->tick1s();
    const std::string row = discovery_rows::rowAt(*rig.dev, "192.0.2.70", false);
    CHECK(std::strstr(row.c_str(), "\"name\":\"Home Assistant\"") != nullptr);
    CHECK(std::strstr(detailFor(*rig.dev, "192.0.2.70").c_str(), "{\"to\":\"192.0.2.1\",\"label\":\"WLED API 6/m\"}") != nullptr);
}
