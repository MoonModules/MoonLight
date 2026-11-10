/// @module DevicesModule

/// The mDNS browse Discovery sends for devices that announce nothing else, and the reply it reads, on a reply captured from a bench ESPHome board.

#include "doctest.h"
#include "core/util/Mdns.h"
#include "mdns_replies.h"

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

TEST_CASE("an ESPHome board's reply names its instance, its friendly name, its version and its board") {
    const std::vector<uint8_t> r = mdns_replies::bytes(mdns_replies::kEsphomeReply);
    mm::mdns::Service s;
    REQUIRE(mm::mdns::parseReply(r.data(), r.size(), "_esphomelib._tcp.local", s));
    CHECK(std::string(s.instance) == "esphome-web-7388c8");
    CHECK(std::string(s.name) == "ESPHome Web 7388c8");
    CHECK(std::string(s.version) == "2026.9.1");
    CHECK(std::string(s.board) == "esp32");
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
