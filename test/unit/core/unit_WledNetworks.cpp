/// @module DevicesModule

/// The WiFi networks a WLED device saved, as the migration image reads them out of WLED's cfg.json and wsec.json.

#include "doctest.h"
#include "core/util/WledNetworks.h"

#include <cstring>
#include <string>

using mm::wlednet::Network;
using mm::wlednet::readNetworks;

TEST_CASE("WLED 0.15's one network is read, not the access point's name that follows it") {
    std::string cfg = R"({"rev":[1,0],"id":{"name":"WLED"},"nw":{"ins":[{"ssid":"Home","pskl":8,"ip":[0,0,0,0],"gw":[0,0,0,0],"sn":[255,255,255,0]}]},"ap":{"ssid":"WLED-AP","chan":1}})";
    std::string sec = R"({"nw":{"ins":[{"psk":"secret12"}]},"ap":{"psk":"wled1234"}})";
    Network nets[4];
    REQUIRE(readNetworks(cfg.data(), sec.data(), nets, 4) == 1);
    CHECK(std::string(nets[0].ssid) == "Home");
    CHECK(std::string(nets[0].psk) == "secret12");
}

TEST_CASE("WLED 16's networks are read in order, an unnamed slot dropped and each password with its own network") {
    // Three slots, the middle one blank, the third with an escaped quote; the arrays inside each entry must not end the list early.
    std::string cfg = R"({"nw":{"ins":[{"ssid":"Home","bssid":"","ip":[0,0,0,0]},{"ssid":"","ip":[0,0,0,0]},{"ssid":"Shed \"2\"","ip":[192,168,1,9]}]},"ap":{"ssid":"WLED-AP"}})";
    std::string sec = R"({"nw":{"ins":[{"psk":"one-pass"},{"psk":""},{"psk":"three-pass"}]},"ap":{"psk":"wled1234"}})";
    Network nets[4];
    REQUIRE(readNetworks(cfg.data(), sec.data(), nets, 4) == 2);
    CHECK(std::string(nets[0].ssid) == "Home");
    CHECK(std::string(nets[0].psk) == "one-pass");
    CHECK(std::string(nets[1].ssid) == "Shed \"2\"");
    CHECK(std::string(nets[1].psk) == "three-pass");
    CHECK(cfg.find("WLED-AP") != std::string::npos);   // the texts are restored as they were
}

TEST_CASE("a WLED with no saved network, or no wsec.json, reads as nothing or as open networks") {
    std::string none = R"({"nw":{"ins":[]},"ap":{"ssid":"WLED-AP"}})";
    Network nets[4];
    CHECK(readNetworks(none.data(), nullptr, nets, 4) == 0);
    std::string cfg = R"({"nw":{"ins":[{"ssid":"Cafe"}]}})";
    REQUIRE(readNetworks(cfg.data(), nullptr, nets, 4) == 1);
    CHECK(std::string(nets[0].psk).empty());
    CHECK(readNetworks(nullptr, nullptr, nets, 4) == 0);
}
