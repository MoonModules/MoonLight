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
}  // namespace

TEST_CASE("the relay's controls exist only on a board that relays, hidden and never saved") {
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

TEST_CASE("a post waits in the outbox as its path and body, and the next is refused until the page empties it") {
    mm::MoonCloudModule cloud(true);
    cloud.defineControls();
    CHECK(cloud.canSend());
    CHECK(cloud.relaysThroughBrowser());
    CHECK(outbox(cloud).empty());

    CHECK(cloud.post("/api/report", "{\"event\":\"install\"}"));
    CHECK(outbox(cloud) == "{\"path\":\"/api/report\",\"body\":{\"event\":\"install\"}}");

    CHECK_FALSE(cloud.post("/api/talk", "{\"text\":\"hi\"}"));   // one slot: the report still waits
    CHECK(outbox(cloud) == "{\"path\":\"/api/report\",\"body\":{\"event\":\"install\"}}");

    CHECK(cloud.outboxWaiting());
    cloud.onControlChanged("outbox sent");
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
