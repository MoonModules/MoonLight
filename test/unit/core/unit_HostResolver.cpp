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

namespace {
/// A non-blocking lookup that answers after `gStepsToAnswer` steps, as a DNS reply arrives some loops later.
int gSteps = 0;
int gStepsToAnswer = 3;
mm::platform::LookupStep fakeStep(const char* name, uint8_t* ip) {
    if (++gSteps < gStepsToAnswer) return mm::platform::LookupStep::Pending;
    gSteps = 0;
    if (std::strcmp(name, "panel.lan") != 0) return mm::platform::LookupStep::Failed;
    ip[0] = 10; ip[1] = 0; ip[2] = 0; ip[3] = 7;
    return mm::platform::LookupStep::Resolved;
}
}  // namespace

TEST_CASE("on a board with no worker task, the asking side looks a name up one non-blocking step per ask") {
    const mm::HostResolver::LookupForTest resolver(&fakeStep);
    gSteps = 0; gStepsToAnswer = 3;
    mm::platform::setTestNowMs(1000);
    uint8_t ip[4] = {};
    CHECK(mm::HostResolver::lookup("panel.lan", ip) == mm::HostResolver::State::Waiting);   // queued
    CHECK(mm::HostResolver::lookup("panel.lan", ip) == mm::HostResolver::State::Waiting);   // step 1: sent
    CHECK(mm::HostResolver::lookup("panel.lan", ip) == mm::HostResolver::State::Waiting);   // step 2: no reply yet
    CHECK(mm::HostResolver::lookup("panel.lan", ip) == mm::HostResolver::State::Resolved);  // step 3: the reply
    CHECK(ip[3] == 7);
    mm::platform::setTestNowMs(0);
}

TEST_CASE("on a board with no worker task, a name that fails is retried only once it is due") {
    const mm::HostResolver::LookupForTest resolver(&fakeStep);
    gSteps = 0; gStepsToAnswer = 1;
    mm::platform::setTestNowMs(1000);
    uint8_t ip[4] = {};
    mm::HostResolver::lookup("nowhere.lan", ip);
    mm::HostResolver::lookup("nowhere.lan", ip);   // looked up, failed
    gSteps = 0; gStepsToAnswer = 100;              // a step now would count
    mm::HostResolver::lookup("nowhere.lan", ip);
    CHECK(gSteps == 0);                            // too soon to retry
    mm::platform::setTestNowMs(7000);              // past the retry interval
    mm::HostResolver::lookup("nowhere.lan", ip);
    CHECK(gSteps == 1);
    mm::platform::setTestNowMs(0);
}
