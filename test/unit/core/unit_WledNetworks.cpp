/// @module DevicesModule

/// The networks a WLED device uses, its saved WiFi and its Ethernet board, as the migration image reads them out of WLED's cfg.json and wsec.json and writes them for MoonBase.

#include "doctest.h"
#include "core/util/WledNetworks.h"
#include "core/util/ConfigScrape.h"   // what MoonBase reads the document with

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

TEST_CASE("a WLED on Ethernet that never saved a network reads as none, its placeholder slot dropped") {
    std::string cfg = R"({"nw":{"ins":[{"ssid":"Your_Network","pskl":0}]}})";   // as WLED-MM on a P4 keeps it
    Network nets[4];
    CHECK(readNetworks(cfg.data(), nullptr, nets, 4) == 0);
}

TEST_CASE("WLED's Ethernet board maps to MoonLight's wiring, and a board it numbers differently or drives with an IP101 does not") {
    mm::wlednet::Ethernet eth = {};
    REQUIRE(mm::wlednet::readEthernet(R"({"nw":{"ins":[]},"eth":{"type":4,"pin":[5,23,18]},"hw":{}})", eth));   // the QuinLED wiring, the Olimex Gateway's
    CHECK(eth.type == 1);   // a LAN8720
    CHECK(eth.rstGpio == 5);
    CHECK(eth.mdcGpio == 23);
    CHECK(eth.mdioGpio == 18);
    CHECK(eth.clockGpio == 17);
    CHECK_FALSE(eth.clockExtIn);
    REQUIRE(mm::wlednet::readEthernet(R"({"eth":{"type":1}})", eth));   // WT32-ETH01: the clock fed in on GPIO 0
    CHECK(eth.clockExtIn);
    CHECK(eth.phyAddr == 1);
    CHECK_FALSE(mm::wlednet::readEthernet(R"({"eth":{"type":0}})", eth));    // none
    CHECK_FALSE(mm::wlednet::readEthernet(R"({"eth":{"type":7}})", eth));    // the Ethernet Kit's IP101
    CHECK_FALSE(mm::wlednet::readEthernet(R"({"eth":{"type":11}})", eth));   // WLED's POE-WROVER, WLED-MM's Olimex Gateway
    CHECK_FALSE(mm::wlednet::readEthernet(R"({"nw":{"ins":[]}})", eth));     // a WLED built without Ethernet writes no eth
    CHECK_FALSE(mm::wlednet::readEthernet(nullptr, eth));
}

TEST_CASE("the network document the migration writes is what MoonBase reads: the Ethernet wiring and every network") {
    const Network nets[1] = {{"Home", "secret12"}};
    mm::wlednet::Ethernet eth = mm::wlednet::kWledEthernet[4];
    char doc[1024];
    const int n = mm::wlednet::networkDocument(doc, sizeof(doc), nets, 1, &eth);
    REQUIRE(n > 0);
    int type = 0, rst = -1, clock = 0;
    bool extIn = true;
    mm::configscrape::findInt(doc, "ethType", &type);
    mm::configscrape::findInt(doc, "ethRstGpio", &rst);
    mm::configscrape::findInt(doc, "ethClockGpio", &clock);
    mm::configscrape::findBool(doc, "ethClockExtIn", &extIn);
    CHECK(type == 1);
    CHECK(rst == 5);
    CHECK(clock == 17);
    CHECK_FALSE(extIn);
    char ssid[16] = {}, pw[16] = {};
    REQUIRE(mm::configscrape::findNetwork(doc, 0, ssid, sizeof(ssid), pw, sizeof(pw)));
    CHECK(std::string(ssid) == "Home");
    CHECK(std::string(pw) == "secret12");
    CHECK(mm::wlednet::networkDocument(doc, 40, nets, 1, &eth) == 0);   // too small a buffer writes nothing usable
    REQUIRE(mm::wlednet::networkDocument(doc, sizeof(doc), nets, 1, nullptr) > 0);
    CHECK(std::strstr(doc, "EthernetModule") == nullptr);   // no Ethernet card for a board on WiFi only
}

TEST_CASE("a saved network name holding a quote, a tab or a newline is escaped, so the document stays valid JSON") {
    const Network nets[1] = {{"Cafe\t\"Lux\"\n2", "pa\x01ss"}};
    char doc[1024];
    REQUIRE(mm::wlednet::networkDocument(doc, sizeof(doc), nets, 1, nullptr) > 0);
    CHECK(std::strstr(doc, R"("ssid":"Cafe\t\"Lux\"\n2")") != nullptr);
    CHECK(std::strstr(doc, R"("password":"pa\u0001ss")") != nullptr);
    for (const char* c = doc; *c; c++) CHECK_MESSAGE(static_cast<unsigned char>(*c) >= 0x20, "a raw control character in the document");
    const Network worst[1] = {{"\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01", ""}};
    CHECK(mm::wlednet::networkDocument(doc, 100, worst, 1, nullptr) == 0);   // an escape that outgrows the buffer writes nothing usable, never a cut one
    const Network shorts[1] = {{"a\bb\fc", "pw"}};   // the escapes with a short form, read back as MoonBase reads them
    REQUIRE(mm::wlednet::networkDocument(doc, sizeof(doc), shorts, 1, nullptr) > 0);
    char ssid[16] = {}, pw[16] = {};
    REQUIRE(mm::configscrape::findNetwork(doc, 0, ssid, sizeof(ssid), pw, sizeof(pw)));
    CHECK(std::string(ssid) == "a\bb\fc");
}

TEST_CASE("WLED's TX power limit carries over in whole dBm, and its maximum carries no limit") {
    CHECK(mm::wlednet::readTxPower(R"({"nw":{"ins":[]},"wifi":{"sleep":false,"phy":0,"txpwr":34}})") == 8);   // 8.5 dBm, a board that browns out at full power
    CHECK(mm::wlednet::readTxPower(R"({"wifi":{"txpwr":78}})") == 0);    // 19.5 dBm, WLED's default: no limit
    CHECK(mm::wlednet::readTxPower(R"({"wifi":{"sleep":true}})") == 0);  // a WLED that saved none
    CHECK(mm::wlednet::readTxPower(nullptr) == 0);
    const Network nets[1] = {{"Home", "secret12"}};
    char doc[1024];
    REQUIRE(mm::wlednet::networkDocument(doc, sizeof(doc), nets, 1, nullptr, 8) > 0);
    int tx = 0;
    mm::configscrape::findInt(doc, "txPowerSetting", &tx);
    CHECK(tx == 8);   // what MoonBase reads before it joins
    REQUIRE(mm::wlednet::networkDocument(doc, sizeof(doc), nets, 1, nullptr, 0) > 0);
    CHECK(std::strstr(doc, "txPowerSetting") == nullptr);
}

TEST_CASE("a Tasmota's network is read out of its settings blob's text pool, entries 4 and 6") {
    // The layout a bench Tasmota 16.0.0 saved: a header, then the text pool at 0x017 holding the OTA URL, the three MQTT prefixes, the two SSIDs and the two passwords.
    std::string blob(mm::wlednet::kTasmotaTextPool, '\x5a');
    const char* pool[] = {"http://ota.tasmota.com/tasmota32/release/tasmota32.bin", "cmnd", "stat", "tele", "Home", "", "secret12", ""};
    for (const char* entry : pool) { blob += entry; blob += '\0'; }
    blob += std::string(64, '\0');
    Network net;
    REQUIRE(mm::wlednet::readTasmotaNetwork(reinterpret_cast<const uint8_t*>(blob.data()), blob.size(), net));
    CHECK(std::string(net.ssid) == "Home");
    CHECK(std::string(net.psk) == "secret12");
    CHECK_FALSE(mm::wlednet::readTasmotaNetwork(reinterpret_cast<const uint8_t*>(blob.data()), 0x20, net));   // cut off before the SSID
    std::string empty(mm::wlednet::kTasmotaTextPool, '\x5a');
    for (int i = 0; i < 8; i++) empty += '\0';
    CHECK_FALSE(mm::wlednet::readTasmotaNetwork(reinterpret_cast<const uint8_t*>(empty.data()), empty.size(), net));   // a Tasmota with no network saved
}

TEST_CASE("an ESPHome device's network is read out of its WiFi preference, a 33-byte SSID and a 65-byte password") {
    // The shape a bench ESPHome Web 2026.9 saved under NVS `esphome`/`88491487` after Improv provisioning.
    std::string blob(mm::wlednet::kEsphomeWifiLen, '\0');
    blob.replace(0, 4, "Home");
    blob.replace(mm::wlednet::kEsphomeSsidLen, 8, "secret12");
    Network net;
    REQUIRE(mm::wlednet::readEsphomeNetwork(reinterpret_cast<const uint8_t*>(blob.data()), blob.size(), net));
    CHECK(std::string(net.ssid) == "Home");
    CHECK(std::string(net.psk) == "secret12");
    CHECK_FALSE(mm::wlednet::readEsphomeNetwork(reinterpret_cast<const uint8_t*>(blob.data()), 40, net));   // too short to be the preference
    std::string none(mm::wlednet::kEsphomeWifiLen, '\0');
    CHECK_FALSE(mm::wlednet::readEsphomeNetwork(reinterpret_cast<const uint8_t*>(none.data()), none.size(), net));
}

TEST_CASE("any other firmware's network is read out of ESP-IDF's own store, a length-prefixed SSID and a NUL-ended password") {
    // The shape ESP-IDF's WiFi library saved under NVS `nvs.net80211` on a bench board: `sta.ssid` 36 bytes, `sta.pswd` 65.
    std::string ssid(mm::wlednet::kIdfSsidLen, '\0');
    ssid[0] = 4;
    ssid.replace(4, 4, "Home");
    std::string psk(65, '\0');
    psk.replace(0, 8, "secret12");
    Network net;
    REQUIRE(mm::wlednet::readIdfNetwork(reinterpret_cast<const uint8_t*>(ssid.data()), ssid.size(), reinterpret_cast<const uint8_t*>(psk.data()), psk.size(), net));
    CHECK(std::string(net.ssid) == "Home");
    CHECK(std::string(net.psk) == "secret12");
    REQUIRE(mm::wlednet::readIdfNetwork(reinterpret_cast<const uint8_t*>(ssid.data()), ssid.size(), nullptr, 0, net));   // an open network saves no password
    CHECK(std::string(net.psk).empty());
    CHECK_FALSE(mm::wlednet::readIdfNetwork(reinterpret_cast<const uint8_t*>(ssid.data()), 20, nullptr, 0, net));   // too short to be the SSID blob
    std::string none(mm::wlednet::kIdfSsidLen, '\0');
    CHECK_FALSE(mm::wlednet::readIdfNetwork(reinterpret_cast<const uint8_t*>(none.data()), none.size(), nullptr, 0, net));   // the store saved no network
    ssid[0] = 33;
    CHECK_FALSE(mm::wlednet::readIdfNetwork(reinterpret_cast<const uint8_t*>(ssid.data()), ssid.size(), nullptr, 0, net));   // a length past the 32 bytes the blob holds
}
