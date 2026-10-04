/// @module NetworkModule
/// @also FilesystemModule

/// The two contracts between the app and MoonBase: the config keys MoonBase scrapes with ConfigScrape.h, and the install routes both images serve.

#include "doctest.h"
#include "core/util/ConfigScrape.h"
#include "network_device.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {
char* textControl(mm::MoonModule& m, const char* name) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, name) == 0) return static_cast<char*>(m.controls()[i].ptr);
    return nullptr;
}
}  // namespace

// The keys MoonBase scrapes are in the files the app writes, read by the scraper MoonBase itself runs.
TEST_CASE("the app's saved config carries every key MoonBase reads") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_moonbase_contract_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");

    auto device = std::make_unique<mm::test::NetworkDevice>(tmpRoot);
    auto& d = *device;
    auto* fs = d.fs;
    auto* sys = d.sys;
    auto* net = d.net;
    auto* ap = d.ap;

    std::strcpy(textControl(*sys, "deviceName"), "MM-bench");
    std::strcpy(textControl(*ap, "password"), "ap-passphrase");
    // A full known list with long passphrases, which puts the access point's keys past 2048 bytes where Ethernet is previewed: why MoonBase reads the whole file.
    for (int i = 0; i < 7; i++) {
        char name[24];
        std::snprintf(name, sizeof(name), "network-%02d", i);
        net->setWifiCredentials(name, "a-passphrase-as-long-as-wpa2-allows-sixty-three-characters-long");
    }
    net->setTxPowerSetting(8);
    net->setWifiCredentials("bench-ssid", "bench-password");   // the first known network
    sys->markDirty();
    net->markDirty();
    fs->flush();

    const auto readFile = [&](const char* name) {
        std::ifstream f(std::string(tmpRoot) + "/.config/" + name);
        REQUIRE(f.good());
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const std::string content = readFile("NetworkModule.json");

    char ssid[64] = {}, password[64] = {};
    REQUIRE(mm::configscrape::findNetwork(content.c_str(), 0, ssid, sizeof(ssid), password, sizeof(password)));
    CHECK(std::string(ssid) == "bench-ssid");
    CHECK(std::string(password) == "bench-password");
    int tx = 0;
    mm::configscrape::findInt(content.c_str(), "txPowerSetting", &tx);
    CHECK(tx == 8);
    // The Ethernet wiring is there too where this build previews it.
    if constexpr (mm::platform::hasEthernet || mm::platform::previewsEthernetControls) {
        int ethType = -2;
        mm::configscrape::findInt(content.c_str(), "ethType", &ethType);
        CHECK(ethType != -2);
    }
    // The access point's own password, not a known network's or another child's.
    char apPassword[64] = {};
    REQUIRE(mm::configscrape::findChildString(content.c_str(), "AccessPointModule", "password", apPassword, sizeof(apPassword)));
    CHECK(std::string(apPassword) == "ap-passphrase");
    // The name MoonBase opens its access point under, the same one the app's carries.
    char name[33] = {};
    REQUIRE(mm::configscrape::findString(readFile("SystemModule.json").c_str(), "deviceName", name, sizeof(name)));
    CHECK(std::string(name) == "MM-bench");

    device.reset();   // released while its folder still exists
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// The scraper matches a key at the top level or under a child, and never inside another key's name.
TEST_CASE("the config scraper finds top-level and child keys, and only whole keys") {
    const char* json = R"({"mDNS":true,"0.type":"EthernetModule","0.ethType":3,"1.known":[{"id":1,"ssid":"a\"b","password":"p"}],"myssid":"no"})";
    int t = 0;
    mm::configscrape::findInt(json, "ethType", &t);
    CHECK(t == 3);
    char ssid[16] = {}, pw[16] = {};
    REQUIRE(mm::configscrape::findNetwork(json, 0, ssid, sizeof(ssid), pw, sizeof(pw)));
    CHECK(std::string(ssid) == "a\"b");
    CHECK(std::string(pw) == "p");
    CHECK_FALSE(mm::configscrape::findNetwork(json, 1, ssid, sizeof(ssid), pw, sizeof(pw)));
    char mine[8] = {};
    CHECK_FALSE(mm::configscrape::findString(json, "ssid2", mine, sizeof(mine)));
    bool b = false;
    mm::configscrape::findBool(json, "mDNS", &b);
    CHECK(b);
}

// MoonBase tries the known networks in the app's order, so a device on its second network at another site still reaches an update.
TEST_CASE("the config scraper reads every known network in order, and a control byte as the app escapes it") {
    const char* json = R"({"1.known":[{"id":1,"ssid":"home","password":"h"},{"id":2,"ssid":"site","password":"s\u0001t"}]})";
    char ssid[16] = {}, pw[16] = {};
    REQUIRE(mm::configscrape::findNetwork(json, 1, ssid, sizeof(ssid), pw, sizeof(pw)));
    CHECK(std::string(ssid) == "site");
    CHECK(std::string(pw) == "s\x01t");
    CHECK_FALSE(mm::configscrape::findNetwork(json, 2, ssid, sizeof(ssid), pw, sizeof(pw)));
    // Past ASCII is refused rather than mis-decoded, which shows as a failed join.
    CHECK_FALSE(mm::configscrape::findString(R"({"ssid":"caf\u00e9"})", "ssid", ssid, sizeof(ssid)));
}

// Known-network and MQTT passwords precede the access point's, and a list row's "type" names no child.
TEST_CASE("the config scraper reads a child's key by the child's type") {
    const char* json = R"({"1.known":[{"ssid":"s","password":"net"}],"4.type":"MqttModule","4.password":"mqtt","2.type":"AccessPointModule","2.password":"ap-pass","5.devices":[{"type":"MoonLight"}]})";
    char out[16] = {};
    REQUIRE(mm::configscrape::findChildString(json, "AccessPointModule", "password", out, sizeof(out)));
    CHECK(std::string(out) == "ap-pass");
    CHECK(mm::configscrape::findChildString(json, "MqttModule", "password", out, sizeof(out)));
    CHECK(std::string(out) == "mqtt");
    CHECK_FALSE(mm::configscrape::findChildString(json, "MoonLight", "password", out, sizeof(out)));
    CHECK_FALSE(mm::configscrape::findChildString(json, "EthernetModule", "password", out, sizeof(out)));
    CHECK_FALSE(mm::configscrape::findChildString(R"({"2.type":"AccessPointModule","2.password":""})", "AccessPointModule", "password", out, sizeof(out)));
}

// The page keeps calling the same paths across the hand-over to MoonBase, which shares no sources, so a renamed route fails only on a device mid-update.
TEST_CASE("the two boot images serve the OTA routes under the same names") {
    // Resolved from this file rather than the working directory: ctest runs the binary from the build tree, where a relative path finds nothing.
    const std::filesystem::path repo =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto read = [&](const char* rel) {
        std::ifstream f(repo / rel);
        REQUIRE_MESSAGE(f.good(), "cannot open " << rel);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const std::string moonbase = read("moonbase/main/moonbase_main.cpp");
    const std::string app      = read("src/core/system/HttpServerModule.cpp");

    // Push an image, and install from a URL: the two routes a browser calls across the handover.
    for (const char* route : {"/api/firmware/upload", "/api/firmware/url"}) {
        CHECK_MESSAGE(moonbase.find(route) != std::string::npos, "MoonBase must serve " << route);
        CHECK_MESSAGE(app.find(route) != std::string::npos, "the app must serve " << route);
    }

    // The old names stay gone: a leftover is a second way to say one thing.
    for (const char* gone : {"\"POST /install\"", "\"POST /install-url\"", "'/install'", "'/install-url'"}) {
        CHECK_MESSAGE(moonbase.find(gone) == std::string::npos, "MoonBase still references " << gone);
    }

    // Each image writes the other's slot, never its own: MoonBase the app slot, the app the factory slot.
    CHECK_MESSAGE(app.find("/api/firmware/moonbase-update") != std::string::npos,
                  "the app must serve the route that installs a new MoonBase");
    CHECK_MESSAGE(moonbase.find("/api/firmware/moonbase-update") == std::string::npos,
                  "MoonBase cannot install itself: it runs from the partition it would erase");

    // An image streams only on the allowlist, else it is refused as too large before its handler runs; checked by name, since a reformat moves the length.
    const size_t allow = app.find("isStreamingRoute");
    REQUIRE(allow != std::string::npos);
    const bool onAllowlist = app.find("moonbase-update", allow) < app.find("bodyNeeded >", allow);
    CHECK_MESSAGE(onAllowlist,
                  "the MoonBase update route must be on the streaming allowlist");

    // A URL the device fetches itself installs a release asset without relaying it through the browser.
    CHECK_MESSAGE(app.find("/api/firmware/moonbase-update-url") != std::string::npos,
                  "the app must also install a MoonBase from a URL");

    // The UI detects MoonBase by a control the module publishes only there, so both name the same one, or the card silently loses its way in.
    const std::string ui = read("src/ui/app.js");
    CHECK_MESSAGE(ui.find("c.name === \"image\"") != std::string::npos,
                  "the UI must detect MoonBase by the control the module actually publishes");
    CHECK_MESSAGE(read("src/core/system/FirmwareUpdateModule.h").find("addSelect(\"image\"") != std::string::npos,
                  "the module must publish that control");

    // Every install shows progress, all six ways in: one with none is indistinguishable from one that hung.
    for (const char* raiser : {"watchInstall(k)", "moonbaseUpdateFlow({ url })",
                               "moonbaseUpdateFlow({ file })", "uploadWithProgress("}) {
        CHECK_MESSAGE(ui.find(raiser) != std::string::npos,
                      "every install path must show progress: missing " << raiser);
    }
    CHECK_MESSAGE(ui.find("showUpdateOverlay()") != std::string::npos,
                  "the progress overlay must exist");

    // Progress rides the status in one shape, read once by the app and once by MoonBase's own page, so both readers are pinned.
    CHECK_MESSAGE(ui.find("function installProgress(") != std::string::npos,
                  "the app must parse install progress in ONE place");
    CHECK_MESSAGE(moonbase.find(") of (") != std::string::npos,
                  "MoonBase's page must read the same 'N of M' shape the app writes");

    // Neither image installs as the other: MoonBase in the app slot leaves no way out but a cable, and either image can be the one writing that slot.
    const std::string ota = read("src/platform/esp32/platform_esp32_ota.cpp");
    CHECK_MESSAGE(ota.find("that is a MoonBase image, not an app") != std::string::npos,
                  "the app install must refuse a MoonBase image");
    CHECK_MESSAGE(moonbase.find("that is a MoonBase image, not an app") != std::string::npos,
                  "MoonBase's install must refuse one too: it writes the app slot");
    CHECK_MESSAGE(moonbase.find("this MoonBase is in the app slot") != std::string::npos,
                  "MoonBase must report, not loop, when it is running from the app slot");

    // MoonBase says which MoonBase it is. Without it, two devices running different builds are indistinguishable, which is what turned one bench failure into a git bisect.
    CHECK_MESSAGE(moonbase.find("/api/version") != std::string::npos,
                  "MoonBase must report its own version");
}
