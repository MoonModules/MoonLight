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
    mm::MoonCloudModule plain(false);
    plain.defineControls();
    CHECK(control(plain, "outbox") == nullptr);
    CHECK(control(plain, "outbox sent") == nullptr);

    mm::MoonCloudModule relay(true);
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
    mm::MoonCloudModule cloud(true);
    cloud.defineControls();
    cloud.setup();
    CHECK(cloud.canSend());
    CHECK(cloud.relaysThroughBrowser());
    CHECK(outbox(cloud).empty());

    CHECK(cloud.post("/api/report", "{\"event\":\"install\"}"));
    CHECK(outbox(cloud) == "{\"id\":1,\"path\":\"/api/report\",\"body\":{\"event\":\"install\"}}");

    CHECK_FALSE(cloud.post("/api/talk", "{\"text\":\"hi\"}"));   // one slot: the report still waits
    CHECK(outbox(cloud) == "{\"id\":1,\"path\":\"/api/report\",\"body\":{\"event\":\"install\"}}");

    CHECK(cloud.outboxWaiting());
    acknowledge(cloud, 1);
    CHECK(outbox(cloud).empty());
    CHECK_FALSE(cloud.outboxWaiting());
    CHECK(cloud.post("/api/talk", "{\"text\":\"hi\"}"));
}

TEST_CASE("a request too large for the outbox is refused rather than truncated") {
    mm::MoonCloudModule cloud(true);
    cloud.defineControls();
    const std::string body = "{\"text\":\"" + std::string(4096, 'x') + "\"}";
    CHECK_FALSE(cloud.post("/api/talk", body.c_str()));
    CHECK(outbox(cloud).empty());
}

// A second open page can acknowledge a request after the first page already did and a newer one took its place: that acknowledgement must not drop the newer request unsent.
TEST_CASE("a late acknowledgement of an earlier request leaves the newer one waiting") {
    mm::MoonCloudModule cloud(true);
    cloud.defineControls();
    cloud.setup();
    REQUIRE(cloud.post("/api/report", "{}"));
    acknowledge(cloud, 1);
    REQUIRE(cloud.post("/api/talk", "{}"));          // id 2
    acknowledge(cloud, 1);                            // the other page, late
    CHECK(cloud.outboxWaiting());
    acknowledge(cloud, 2);
    CHECK_FALSE(cloud.outboxWaiting());
}

// The acknowledgement is a saved value, so a boot restores it: the ids count on past it, or the restored value could match and empty a request no page delivered.
TEST_CASE("after a reboot the ids count on from the last acknowledged one") {
    mm::MoonCloudModule cloud(true);
    cloud.defineControls();
    *static_cast<int32_t*>(const_cast<void*>(control(cloud, "outbox sent")->ptr)) = 7;   // what the settings file restored
    cloud.setup();
    REQUIRE(cloud.post("/api/report", "{}"));
    CHECK(outbox(cloud).rfind("{\"id\":8,", 0) == 0);
    acknowledge(cloud, 7);
    CHECK(cloud.outboxWaiting());
}
