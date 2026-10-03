/// @module HostResolver

/// Names answer from a cache at once while lookups happen elsewhere: waiting until the first answer, then the address, then the last known one while lookups fail.

#include "doctest.h"
#include "core/util/HostResolver.h"

#include <cstring>

namespace {
/// A resolver whose answer a test sets: up and at which last octet, or down.
bool gUp = true;
uint8_t gLast = 40;
int gCalls = 0;
bool fakeResolve(const char* name, uint8_t* ip) {
    gCalls++;
    if (!gUp || std::strcmp(name, "panel.local") != 0) return false;
    ip[0] = 192; ip[1] = 168; ip[2] = 1; ip[3] = gLast;
    return true;
}
}  // namespace

TEST_CASE("a host name waits for its first lookup, then answers its address") {
    const mm::HostResolver::LookupForTest resolver(&fakeResolve);
    gUp = true; gLast = 40;
    uint8_t ip[4] = {};
    CHECK(mm::HostResolver::lookup("panel.local", ip) == mm::HostResolver::State::Waiting);
    mm::HostResolver::passForTest();
    CHECK(mm::HostResolver::lookup("panel.local", ip) == mm::HostResolver::State::Resolved);
    CHECK(ip[3] == 40);
}

TEST_CASE("a host name that stops resolving keeps its last known address") {
    const mm::HostResolver::LookupForTest resolver(&fakeResolve);
    gUp = true; gLast = 41;
    mm::platform::setTestNowMs(1000);
    uint8_t ip[4] = {};
    mm::HostResolver::lookup("panel.local", ip);
    mm::HostResolver::passForTest();
    gUp = false;
    mm::platform::setTestNowMs(62000);   // past the refresh interval
    mm::HostResolver::passForTest();
    CHECK(mm::HostResolver::lookup("panel.local", ip) == mm::HostResolver::State::Stale);
    CHECK(ip[3] == 41);   // still sending to where it was
    gUp = true;
    mm::platform::setTestNowMs(0);
}

TEST_CASE("a host name that never resolves stays waiting, and costs no lookup until it is due") {
    const mm::HostResolver::LookupForTest resolver(&fakeResolve);
    mm::platform::setTestNowMs(1000);
    uint8_t ip[4] = {};
    mm::HostResolver::lookup("nowhere.lan", ip);
    gCalls = 0;
    mm::HostResolver::passForTest();
    mm::HostResolver::passForTest();   // too soon to retry
    CHECK(gCalls == 1);
    CHECK(mm::HostResolver::lookup("nowhere.lan", ip) == mm::HostResolver::State::Waiting);
    mm::platform::setTestNowMs(0);
}
