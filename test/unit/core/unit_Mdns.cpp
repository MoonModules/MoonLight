/// @module DevicesModule

/// The mDNS browse Discovery sends for devices that announce nothing else, and the reply it reads, on a reply captured from a bench ESPHome board.

#include "doctest.h"
#include "core/util/Mdns.h"
#include "mdns_replies.h"

#include <cstring>
#include <string>
#include <vector>


TEST_CASE("the browse is one PTR question for the service, as RFC 6763 asks it") {
    uint8_t q[64];
    const size_t n = mm::mdns::buildQuery("_esphomelib._tcp.local", q, sizeof(q));
    REQUIRE(n == 12 + 1 + 11 + 1 + 4 + 1 + 5 + 1 + 4);
    CHECK(q[5] == 1);                                       // one question
    CHECK(std::string(reinterpret_cast<char*>(q + 13), 11) == "_esphomelib");
    CHECK(q[n - 3] == mm::mdns::kTypePtr);
    CHECK(mm::mdns::buildQuery("_esphomelib._tcp.local", q, 20) == 0);   // too small a buffer writes nothing usable
}

TEST_CASE("an ESPHome board's reply names its instance, its friendly name, its version, its board and its MAC") {
    const std::vector<uint8_t> r = mdns_replies::bytes(mdns_replies::kEsphomeReply);
    mm::mdns::Service s;
    REQUIRE(mm::mdns::parseReply(r.data(), r.size(), "_esphomelib._tcp.local", s));
    CHECK(std::string(s.instance) == "esphome-web-7388c8");
    CHECK(std::string(s.name) == "ESPHome Web 7388c8");
    CHECK(std::string(s.version) == "2026.9.1");
    CHECK(std::string(s.board) == "esp32");
    const uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0x73, 0x88, 0xc8};
    CHECK(std::memcmp(s.mac, mac, 6) == 0);
    CHECK(std::string(s.chip) == "esp32");   // platform ESP32, board esp32: the classic
}

TEST_CASE("an ESPHome record's chip comes from its variant, else its board's name, and is none for a platform MoonLight does not run on") {
    char chip[10];
    chip[0] = 0; mm::mdns::esphomeChip("ESP32", "ESP32S3", "esp32dev", chip, sizeof(chip)); CHECK(std::string(chip) == "esp32s3");
    chip[0] = 0; mm::mdns::esphomeChip("ESP32", "", "esp32-c3-devkitm-1", chip, sizeof(chip)); CHECK(std::string(chip) == "esp32c3");
    chip[0] = 0; mm::mdns::esphomeChip("ESP32", "", "esp32dev", chip, sizeof(chip)); CHECK(std::string(chip) == "esp32");
    chip[0] = 0; mm::mdns::esphomeChip("ESP8266", "", "d1_mini", chip, sizeof(chip)); CHECK(std::string(chip).empty());
    chip[0] = 0; mm::mdns::esphomeChip("ESP8266", "ESP8266EX", "d1_mini", chip, sizeof(chip)); CHECK(std::string(chip).empty());   // a variant on another platform
    chip[0] = 0; mm::mdns::esphomeChip("ESP32", "ESP32S31", "", chip, sizeof(chip)); CHECK(std::string(chip) == "esp32s31");    // the S31, not the S3 its name begins with
    chip[0] = 0; mm::mdns::esphomeChip("", "ESP32C3", "", chip, sizeof(chip)); CHECK(std::string(chip) == "esp32c3");          // an older record without a platform
}

TEST_CASE("a WLED board's reply names its MAC, the identity its presence packet lacks") {
    const std::vector<uint8_t> r = mdns_replies::bytes(mdns_replies::kWledReply);
    mm::mdns::Service s;
    REQUIRE(mm::mdns::parseReply(r.data(), r.size(), "_wled._tcp.local", s));
    CHECK(std::string(s.instance) == "wled-e15ceb");
    const uint8_t mac[6] = {0x30, 0xed, 0xa0, 0xe1, 0x5c, 0xeb};
    CHECK(std::memcmp(s.mac, mac, 6) == 0);
    CHECK_FALSE(mm::mdns::parseReply(r.data(), r.size(), "_esphomelib._tcp.local", s));   // the other browse's reply names nothing
}

TEST_CASE("a MAC reads as twelve hex digits, with or without separators, and nothing else does") {
    uint8_t m[6] = {};
    REQUIRE(mm::mdns::parseMac("08b61f6fbd3c", m));
    CHECK(m[0] == 0x08);
    CHECK(m[5] == 0x3c);
    REQUIRE(mm::mdns::parseMac("08:B6:1F:6F:BD:3C", m));
    CHECK(m[1] == 0xb6);
    CHECK(mm::mdns::parseMac("08-b6-1f-6f-bd-3c", m));
    CHECK_FALSE(mm::mdns::parseMac("08b61f6fbd", m));       // short
    CHECK_FALSE(mm::mdns::parseMac("08b61f6fbd3c00", m));   // long
    CHECK_FALSE(mm::mdns::parseMac("08b61f6fbdzz", m));     // not hex
    CHECK_FALSE(mm::mdns::parseMac("", m));
}

TEST_CASE("a reply for another service, a question, or a cut-off reply names nothing, and never reads past its end") {
    const std::vector<uint8_t> r = mdns_replies::bytes(mdns_replies::kEsphomeReply);
    mm::mdns::Service s;
    CHECK_FALSE(mm::mdns::parseReply(r.data(), r.size(), "_hue._tcp.local", s));
    uint8_t q[64];
    const size_t n = mm::mdns::buildQuery("_esphomelib._tcp.local", q, sizeof(q));
    CHECK_FALSE(mm::mdns::parseReply(q, n, "_esphomelib._tcp.local", s));
    for (size_t cut = 0; cut < r.size(); cut += 7) mm::mdns::parseReply(r.data(), cut, "_esphomelib._tcp.local", s);   // any length, no crash
}

TEST_CASE("a reply whose names point forward or in a loop names nothing, as RFC 1035 lets a pointer name only an earlier name") {
    // Header (a response, one answer), then a PTR whose owner is a pointer to itself: a loop.
    std::vector<uint8_t> loop = {0x4d, 0x4d, 0x84, 0x00, 0, 0, 0, 1, 0, 0, 0, 0, 0xc0, 12, 0, 12, 0, 1, 0, 0, 0, 120, 0, 2, 0xc0, 12};
    mm::mdns::Service s;
    CHECK_FALSE(mm::mdns::parseReply(loop.data(), loop.size(), "_esphomelib._tcp.local", s));
    char out[32];
    CHECK(mm::mdns::readName(loop.data(), loop.size(), 12, out, sizeof(out)) == 0);
    // A pointer forward, past itself, to a name later in the message.
    std::vector<uint8_t> fwd = {0x4d, 0x4d, 0x84, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 14, 3, 'a', 'b', 'c', 0};
    CHECK(mm::mdns::readName(fwd.data(), fwd.size(), 12, out, sizeof(out)) == 0);
    CHECK(mm::mdns::readName(fwd.data(), fwd.size(), 14, out, sizeof(out)) == 19);   // the name itself reads
    CHECK(std::string(out) == "abc");
}
