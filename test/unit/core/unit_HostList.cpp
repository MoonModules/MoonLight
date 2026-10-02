/// @module HostList

/// The host-list parser: addresses with their ranges and shorthand, names, and both mixed, a typo refused rather than sent somewhere.

#include "doctest.h"
#include "core/util/HostList.h"

#include <string>

namespace {
// Compact assertion helper: "is host i the address a.b.c.d?"
bool is(const mm::Host& h, int a, int b, int c, int d) {
    return !h.isName() && h.ip[0] == a && h.ip[1] == b && h.ip[2] == c && h.ip[3] == d;
}
std::string nameOf(const char* text, const mm::Host& h) {
    char buf[mm::kMaxHostName + 1];
    mm::hostName(text, h, buf);
    return buf;
}
}  // namespace

TEST_CASE("a host list range expands over the last octet") {
    mm::Host hosts[32];
    uint8_t n = 0;
    REQUIRE(mm::parseHostList("192.168.1.60-63", hosts, 32, n) == nullptr);
    CHECK(n == 4);                            // 60, 61, 62, 63, inclusive at both ends
    CHECK(is(hosts[0], 192, 168, 1, 60));
    CHECK(is(hosts[3], 192, 168, 1, 63));
}

TEST_CASE("a host list of bare host numbers continues the same subnet") {
    mm::Host hosts[32];
    uint8_t n = 0;
    REQUIRE(mm::parseHostList("192.168.1.60,61,62,65", hosts, 32, n) == nullptr);
    CHECK(n == 4);
    CHECK(is(hosts[0], 192, 168, 1, 60));
    CHECK(is(hosts[3], 192, 168, 1, 65));
}

TEST_CASE("a host list's full quads may switch subnet, and ranges and lists mix") {
    mm::Host hosts[32];
    uint8_t n = 0;
    REQUIRE(mm::parseHostList("192.168.1.60-61, 10.0.0.5, 6", hosts, 32, n) == nullptr);
    CHECK(n == 4);
    CHECK(is(hosts[1], 192, 168, 1, 61));
    CHECK(is(hosts[2], 10, 0, 0, 5));
    CHECK(is(hosts[3], 10, 0, 0, 6));
}

TEST_CASE("a host list takes names and mixes them with addresses") {
    const char* text = "192.168.1.10-11, panel-01.local, stage.lan, 12";
    mm::Host hosts[32];
    uint8_t n = 0;
    REQUIRE(mm::parseHostList(text, hosts, 32, n) == nullptr);
    REQUIRE(n == 5);
    CHECK(is(hosts[0], 192, 168, 1, 10));
    CHECK(hosts[2].isName());
    CHECK(nameOf(text, hosts[2]) == "panel-01.local");
    CHECK(nameOf(text, hosts[3]) == "stage.lan");
    // A name resolves later, so it holds no address yet.
    CHECK((hosts[2].ip[0] | hosts[2].ip[1] | hosts[2].ip[2] | hosts[2].ip[3]) == 0);
    // The last-octet shorthand continues the last ADDRESS, across the names.
    CHECK(is(hosts[4], 192, 168, 1, 12));
}

TEST_CASE("a blank host list is no hosts, which leaves the service idle") {
    mm::Host hosts[32];
    uint8_t n = 9;
    CHECK(mm::parseHostList("", hosts, 32, n) == nullptr);
    CHECK(n == 0);
    CHECK(mm::parseHostList("   ", hosts, 32, n) == nullptr);
    CHECK(mm::parseHostList(nullptr, hosts, 32, n) == nullptr);
}

TEST_CASE("a malformed host list is refused, never guessed at") {
    mm::Host hosts[32];
    uint8_t n = 0;
    CHECK(mm::parseHostList("192.168.1.999", hosts, 32, n) != nullptr);   // octet > 255, not a name
    CHECK(mm::parseHostList("192.168.1.60,,61", hosts, 32, n) != nullptr); // empty entry
    CHECK(mm::parseHostList("192.168.1.60-", hosts, 32, n) != nullptr);    // range with no end
    CHECK(mm::parseHostList("192.168.1.70-60", hosts, 32, n) != nullptr);  // backwards range
    CHECK(mm::parseHostList("60,61", hosts, 32, n) != nullptr);            // bare number, no subnet yet
    CHECK(mm::parseHostList("panel 01", hosts, 32, n) != nullptr);         // a space inside a name
    CHECK(mm::parseHostList("-panel.local", hosts, 32, n) != nullptr);     // a name cannot start with a dash
}

TEST_CASE("a host list's cap is enforced, not silently truncated") {
    mm::Host hosts[4];
    uint8_t n = 0;
    CHECK(mm::parseHostList("192.168.1.1-100", hosts, 4, n) != nullptr);
    CHECK(mm::parseHostList("a.local, b.local, c.local, d.local, e.local", hosts, 4, n) != nullptr);
}
