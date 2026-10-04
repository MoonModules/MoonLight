/// @module NetworkModule
/// @also FilesystemModule, EthernetModule, WiFiModule

/// A device updated in place from 6.0 keeps its network: 6.0's top-level settings move onto the Ethernet and WiFi cards at the first boot.

#include "doctest.h"
#include "network_device.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// What 6.0 wrote for a device on a static address with a hand-wired LAN8720: everything at Network's top level, the board by its label.
const char* kSixZeroNetwork =
    R"({"ssid":"old-home","password":"old-password","txPowerSetting":8,"mDNS":true,"addressing":1,)"
    R"("ip":"192.168.1.9","gateway":"192.168.1.1","subnet":"255.255.255.0","dns":"192.168.1.1",)"
    R"("ethBoard":"Custom","ethType":1,"ethPhyAddr":0,"ethMdcGpio":16,"ethMdioGpio":17,"enabled":true})";

// The same on a named board whose saved reset pin is stale: 6.0 wrote the board's map over it at every boot.
const char* kSixZeroNamedBoard =
    R"json({"ethBoard":"Classic RMII (no reset)","ethType":1,"ethPhyAddr":0,"ethRstGpio":5,"ethMdcGpio":23,"ethMdioGpio":18,"enabled":true})json";

const void* controlPtr(mm::MoonModule& m, const char* name) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, name) == 0) return m.controls()[i].ptr;
    return nullptr;
}

using Device = mm::test::NetworkDevice;

std::string readConfig(const std::string& root) {
    std::ifstream in(root + "/.config/NetworkModule.json");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string freshRoot(const char* json) {
    const std::string root = "/tmp/mm_network_legacy_" + std::to_string(mm::platform::millis());
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/.config");
    std::ofstream(root + "/.config/NetworkModule.json") << json;
    return root;
}

int8_t pin(mm::MoonModule& eth, const char* name) { return *static_cast<const int8_t*>(controlPtr(eth, name)); }

}  // namespace

TEST_CASE("a config from 6.0 moves its network onto the Ethernet and WiFi cards, once") {
    const std::string root = freshRoot(kSixZeroNetwork);
    {
        Device d(root.c_str());
        REQUIRE(d.wifi->knownCount() == 1);
        CHECK(std::strcmp(d.wifi->ssidAt(0), "old-home") == 0);
        CHECK(std::strcmp(d.wifi->passwordAt(0), "old-password") == 0);
        const uint8_t* ip = d.wifi->staticIpAt(0);
        REQUIRE(ip);
        CHECK((ip[0] == 192 && ip[1] == 168 && ip[2] == 1 && ip[3] == 9));
        CHECK(*static_cast<const int16_t*>(controlPtr(*d.wifi, "txPowerSetting")) == 8);
        CHECK(*static_cast<const uint8_t*>(controlPtr(*d.eth, "ethType")) == 1);
        CHECK(*static_cast<const uint8_t*>(controlPtr(*d.eth, "ipSettings")) == 1);
        // Custom keeps the hand-wired pins through the rebuild that follows a board change.
        d.eth->rebuildControls();
        CHECK(pin(*d.eth, "ethMdcGpio") == 16);
        CHECK(pin(*d.eth, "ethMdioGpio") == 17);
        d.fs->flush();
    }
    const std::string saved = readConfig(root);
    CHECK(saved.find("old-home") != std::string::npos);        // in the WiFi card's known list
    CHECK(saved.find("\"addressing\"") == std::string::npos);  // 6.0's shared key is gone
    CHECK(saved.find("\"ethType\":") == std::string::npos);    // only as the Ethernet card's "0.ethType", which hasKey("ethType") does not match
    // A second boot finds nothing to move, so it saves nothing new.
    {
        Device again(root.c_str());
        again.fs->flush();
    }
    CHECK(readConfig(root) == saved);
    std::filesystem::remove_all(root);
    mm::platform::fsSetRoot(".");
}

TEST_CASE("a 6.0 config on a named Ethernet board takes the board's map, as 6.0 did at boot") {
    const std::string root = freshRoot(kSixZeroNamedBoard);
    {
        Device d(root.c_str());
        d.eth->rebuildControls();
        CHECK(pin(*d.eth, "ethRstGpio") == -1);   // the map's, not the stale saved 5
        CHECK(pin(*d.eth, "ethMdcGpio") == 23);
        CHECK(d.wifi->knownCount() == 0);          // no network to move
    }
    std::filesystem::remove_all(root);
    mm::platform::fsSetRoot(".");
}
