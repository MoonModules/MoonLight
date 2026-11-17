/// @module ReceiverList

/// The receiver-list parser: addresses with their ranges and shorthand, names, and both mixed, a typo refused rather than sent somewhere.

#include "doctest.h"
#include "core/util/ReceiverList.h"

#include <string>

namespace {
// Compact assertion helper: "is receiver i the address a.b.c.d?"
bool is(const mm::Receiver& r, int a, int b, int c, int d) {
    return !r.isName() && r.ip[0] == a && r.ip[1] == b && r.ip[2] == c && r.ip[3] == d;
}
std::string nameOf(const char* text, const mm::Receiver& r) {
    char buf[mm::kMaxHostName + 1];
    mm::receiverName(text, r, buf);
    return buf;
}
}  // namespace

TEST_CASE("a receiver list range expands over the last octet") {
    mm::Receiver receivers[32];
    uint8_t n = 0;
    REQUIRE(mm::parseReceiverList("192.168.1.60-63", receivers, 32, n) == nullptr);
    CHECK(n == 4);                            // 60, 61, 62, 63, inclusive at both ends
    CHECK(is(receivers[0], 192, 168, 1, 60));
    CHECK(is(receivers[3], 192, 168, 1, 63));
}

TEST_CASE("a receiver list of bare receiver numbers continues the same subnet") {
    mm::Receiver receivers[32];
    uint8_t n = 0;
    REQUIRE(mm::parseReceiverList("192.168.1.60,61,62,65", receivers, 32, n) == nullptr);
    CHECK(n == 4);
    CHECK(is(receivers[0], 192, 168, 1, 60));
    CHECK(is(receivers[3], 192, 168, 1, 65));
}

TEST_CASE("a receiver list's full quads may switch subnet, and ranges and lists mix") {
    mm::Receiver receivers[32];
    uint8_t n = 0;
    REQUIRE(mm::parseReceiverList("192.168.1.60-61, 10.0.0.5, 6", receivers, 32, n) == nullptr);
    CHECK(n == 4);
    CHECK(is(receivers[1], 192, 168, 1, 61));
    CHECK(is(receivers[2], 10, 0, 0, 5));
    CHECK(is(receivers[3], 10, 0, 0, 6));
}

TEST_CASE("a receiver list takes names and mixes them with addresses") {
    const char* text = "192.168.1.10-11, panel-01.local, stage.lan, 12";
    mm::Receiver receivers[32];
    uint8_t n = 0;
    REQUIRE(mm::parseReceiverList(text, receivers, 32, n) == nullptr);
    REQUIRE(n == 5);
    CHECK(is(receivers[0], 192, 168, 1, 10));
    CHECK(receivers[2].isName());
    CHECK(nameOf(text, receivers[2]) == "panel-01.local");
    CHECK(nameOf(text, receivers[3]) == "stage.lan");
    // A name resolves later, so it holds no address yet.
    CHECK((receivers[2].ip[0] | receivers[2].ip[1] | receivers[2].ip[2] | receivers[2].ip[3]) == 0);
    // The last-octet shorthand continues the last ADDRESS, across the names.
    CHECK(is(receivers[4], 192, 168, 1, 12));
}

TEST_CASE("a blank receiver list is no receivers, which leaves the service idle") {
    mm::Receiver receivers[32];
    uint8_t n = 9;
    CHECK(mm::parseReceiverList("", receivers, 32, n) == nullptr);
    CHECK(n == 0);
    CHECK(mm::parseReceiverList("   ", receivers, 32, n) == nullptr);
    CHECK(mm::parseReceiverList(nullptr, receivers, 32, n) == nullptr);
}

TEST_CASE("a malformed receiver list is refused, never guessed at") {
    mm::Receiver receivers[32];
    uint8_t n = 0;
    CHECK(mm::parseReceiverList("192.168.1.999", receivers, 32, n) != nullptr);   // octet > 255, not a name
    CHECK(mm::parseReceiverList("192.168.1.60,,61", receivers, 32, n) != nullptr); // empty entry
    CHECK(mm::parseReceiverList("192.168.1.60-", receivers, 32, n) != nullptr);    // range with no end
    CHECK(mm::parseReceiverList("192.168.1.70-60", receivers, 32, n) != nullptr);  // backwards range
    CHECK(mm::parseReceiverList("60,61", receivers, 32, n) != nullptr);            // bare number, no subnet yet
    CHECK(mm::parseReceiverList("panel 01", receivers, 32, n) != nullptr);         // a space inside a name
    CHECK(mm::parseReceiverList("-panel.local", receivers, 32, n) != nullptr);     // a name cannot start with a dash
}

TEST_CASE("a receiver list's cap is enforced, not silently truncated") {
    mm::Receiver receivers[4];
    uint8_t n = 0;
    CHECK(mm::parseReceiverList("192.168.1.1-100", receivers, 4, n) != nullptr);
    CHECK(mm::parseReceiverList("a.local, b.local, c.local, d.local, e.local", receivers, 4, n) != nullptr);
}
