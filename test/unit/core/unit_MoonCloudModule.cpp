/// @module MoonCloudModule

/// The browser relay: a board without HTTPS hands its one MoonCloud request to the open page through a hidden outbox control.

#include "doctest.h"
#include "core/system/MoonCloudModule.h"

#include <cstring>
#include <string>

namespace {
const mm::ControlDescriptor* control(const mm::MoonModule& m, const char* name) {
    const auto& cs = m.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (cs[i].name && std::strcmp(cs[i].name, name) == 0) return &cs[i];
    return nullptr;
}
std::string outbox(const mm::MoonModule& m) {
    const auto* c = control(m, "outbox");
    return c ? static_cast<const char*>(c->ptr) : "<none>";
}
// What the page does once it has posted a request: write that request's id to `outbox sent`.
void acknowledge(mm::MoonModule& m, int32_t id) {
    *static_cast<int32_t*>(const_cast<void*>(control(m, "outbox sent")->ptr)) = id;
    m.onControlChanged("outbox sent");
}
}  // namespace

TEST_CASE("the relay's controls exist only on a board that relays, hidden, the request itself never saved") {
    mm::MoonCloudModule plain(mm::MoonCloudModule::Route::Direct);
    plain.defineControls();
    CHECK(control(plain, "outbox") == nullptr);
    CHECK(control(plain, "outbox sent") == nullptr);

    mm::MoonCloudModule relay(mm::MoonCloudModule::Route::Page);
    relay.defineControls();
    const auto* box = control(relay, "outbox");
    REQUIRE(box != nullptr);
    CHECK(box->hidden);
    CHECK(box->readonly);   // read-only is what keeps a transient request out of the saved settings
    const auto* sent = control(relay, "outbox sent");
    REQUIRE(sent != nullptr);
    CHECK(sent->hidden);
}

TEST_CASE("a post waits in the outbox as its id, path and body, and the next is refused until the page empties it") {
    mm::MoonCloudModule cloud(mm::MoonCloudModule::Route::Page);
    cloud.defineControls();
    cloud.setup();
    CHECK(cloud.canSend());
    CHECK(cloud.relaysThroughBrowser());
    CHECK(outbox(cloud).empty());

    CHECK(cloud.post("/api/report", "{\"event\":\"install\"}"));
    CHECK(outbox(cloud) == "{\"id\":1,\"path\":\"/api/report\",\"body\":{\"event\":\"install\"}}");

    CHECK_FALSE(cloud.post("/api/talk", "{\"text\":\"hi\"}"));   // one slot: the report still waits
    CHECK(outbox(cloud) == "{\"id\":1,\"path\":\"/api/report\",\"body\":{\"event\":\"install\"}}");

    CHECK(cloud.waiting());
    acknowledge(cloud, 1);
    CHECK(outbox(cloud).empty());
    CHECK_FALSE(cloud.waiting());
    CHECK(cloud.post("/api/talk", "{\"text\":\"hi\"}"));
}

TEST_CASE("a request too large for the outbox is refused rather than truncated") {
    mm::MoonCloudModule cloud(mm::MoonCloudModule::Route::Page);
    cloud.defineControls();
    const std::string body = "{\"text\":\"" + std::string(4096, 'x') + "\"}";
    CHECK_FALSE(cloud.post("/api/talk", body.c_str()));
    CHECK(outbox(cloud).empty());
}

// A second open page can acknowledge a request after the first page already did and a newer one took its place: that acknowledgement must not drop the newer request unsent.
TEST_CASE("a late acknowledgement of an earlier request leaves the newer one waiting") {
    mm::MoonCloudModule cloud(mm::MoonCloudModule::Route::Page);
    cloud.defineControls();
    cloud.setup();
    REQUIRE(cloud.post("/api/report", "{}"));
    acknowledge(cloud, 1);
    REQUIRE(cloud.post("/api/talk", "{}"));          // id 2
    acknowledge(cloud, 1);                            // the other page, late
    CHECK(cloud.waiting());
    acknowledge(cloud, 2);
    CHECK_FALSE(cloud.waiting());
}

// The acknowledgement is a saved value, so a boot restores it: the ids count on past it, or the restored value could match and empty a request no page delivered.
TEST_CASE("after a reboot the ids count on from the last acknowledged one") {
    mm::MoonCloudModule cloud(mm::MoonCloudModule::Route::Page);
    cloud.defineControls();
    *static_cast<int32_t*>(const_cast<void*>(control(cloud, "outbox sent")->ptr)) = 7;   // what the settings file restored
    cloud.setup();
    REQUIRE(cloud.post("/api/report", "{}"));
    CHECK(outbox(cloud).rfind("{\"id\":8,", 0) == 0);
    acknowledge(cloud, 7);
    CHECK(cloud.waiting());
}

namespace {
int g_direct = 0;
int directStandIn(const char*, const char*, const char*, char*, size_t) { g_direct++; return 200; }
}  // namespace

/// A board without PSRAM sends itself while a TLS session fits, and hands the request to the open page when one does not, so its report still arrives.
TEST_CASE("without PSRAM a post goes out of the board while TLS fits, and to the page while it does not") {
    if (!mm::platform::httpsAvailable()) { MESSAGE("no HTTPS client in this build"); return; }
    mm::platform::setTestHttpFetch(&directStandIn);
    g_direct = 0;
    mm::MoonCloudModule cloud(mm::MoonCloudModule::Route::DirectOrPage);
    cloud.defineControls();
    CHECK(control(cloud, "outbox") != nullptr);      // the page's way is there, hidden
    CHECK_FALSE(cloud.relaysThroughBrowser());       // though the board has its own client

    // Room for a session: out of the board, on the worker.
    const uint32_t first = cloud.post("/api/report", "{}");
    REQUIRE(first);
    CHECK_FALSE(cloud.lastViaPage());
    REQUIRE(mm::JsonFetch::instance().settle(2000));
    CHECK_FALSE(cloud.waiting(first));
    CHECK(cloud.delivered(first));
    CHECK(g_direct == 1);

    // Internal RAM under the floor plus a session: to the page instead.
    mm::platform::setTestFreeInternalHeap(mm::platform::kInternalFreeFloor);
    REQUIRE(cloud.post("/api/report", "{}"));
    CHECK(cloud.lastViaPage());
    CHECK(cloud.waiting());
    CHECK(g_direct == 1);                            // nothing left the board
    mm::platform::setTestFreeInternalHeap(0);
    mm::platform::setTestHttpFetch(nullptr);
}

/// Two modules send through one MoonCloud, so each reads the outcome of its own ticket, never the other's.
TEST_CASE("each sender reads its own request's outcome, and an older ticket's reads as not arrived") {
    mm::MoonCloudModule cloud(mm::MoonCloudModule::Route::Page);
    cloud.defineControls();
    const uint32_t report = cloud.post("/api/report", "{}");
    REQUIRE(report);
    CHECK(cloud.waiting(report));
    acknowledge(cloud, 1);
    CHECK(cloud.delivered(report));
    const uint32_t talk = cloud.post("/api/talk", "{}");
    REQUIRE(talk);
    CHECK(talk != report);
    CHECK_FALSE(cloud.waiting(report));
    CHECK(cloud.waiting(talk));
    CHECK_FALSE(cloud.delivered(talk));
    acknowledge(cloud, 2);
    CHECK(cloud.delivered(talk));
    CHECK_FALSE(cloud.delivered(report));   // its outcome was settled before, and is gone
}
