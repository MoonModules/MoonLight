/// @module DevicesModule

/// ESPHome's native update protocol from the client's side, run against a scripted device the way ESPHome's own client and server exchange it.

#include "doctest.h"
#include "core/util/EsphomeOta.h"

#include <cstring>
#include <string>

using mm::esphomeota::Client;

namespace {
// Everything the client has to send now, taken as if the socket took it whole.
std::string drain(Client& c) {
    size_t n = 0;
    const uint8_t* p = c.pending(n);
    std::string out(reinterpret_cast<const char*>(p), n);
    c.sent(n);
    return out;
}
const char* kMd5 = "0123456789abcdef0123456789abcdef";
}  // namespace

TEST_CASE("an update without a password runs ESPHome's handshake, passes the image through and closes on the device's commit") {
    Client c;
    c.begin(20000, kMd5);
    CHECK(drain(c) == std::string("\x6C\x26\xF7\x5C\x45", 5));   // the magic bytes
    c.received(0x00); c.received(0x02);                          // OK, protocol version 2
    CHECK(c.protocolVersion() == 2);
    CHECK(drain(c) == std::string("\x00", 1));                   // no features asked for
    c.received(0x40);                                            // header OK: the plain protocol
    c.received(0x41);                                            // no password
    CHECK(drain(c) == std::string("\x00\x00\x4E\x20", 4));       // 20000, big-endian
    c.received(0x42);                                            // prepared
    CHECK(drain(c) == kMd5);
    c.received(0x43);                                            // MD5 taken
    REQUIRE(c.wantsData());
    c.dataSent(8192); c.received(0x47);                          // a chunk OK per 8 KB crossed
    c.dataSent(8192); c.received(0x47);
    c.dataSent(20000 - 16384);
    CHECK_FALSE(c.wantsData());
    c.received(0x47);                                            // the remainder's
    c.received(0x44); c.received(0x45);                          // received, update end
    CHECK(c.acks() == 3);
    CHECK(drain(c) == std::string("\x00", 1));                   // our closing OK
    CHECK(c.state() == Client::State::Done);
}

TEST_CASE("a device asking for a password, or answering with an error code, ends the update with its reason") {
    Client c;
    c.begin(100, kMd5);
    drain(c);
    c.received(0x00); c.received(0x02); drain(c);
    c.received(0x48); c.received(0x00);   // the extended answer: a flags byte follows, skipped
    c.received(0x02);                     // SHA-256 password requested
    CHECK(c.state() == Client::State::Failed);
    CHECK(std::string(c.error()) == "the device asks for a password");

    Client d;
    d.begin(100, kMd5);
    drain(d);
    d.received(0x00); d.received(0x01); drain(d);
    d.received(0x40); d.received(0x41); drain(d);
    d.received(0x89);                     // not enough space for the image
    CHECK(d.state() == Client::State::Failed);
    CHECK(d.errorCode() == 0x89);

    Client e;
    e.begin(100, kMd5);
    drain(e);
    e.received(0x48);                     // not ESPHome's answer to the magic bytes
    CHECK(e.state() == Client::State::Failed);
}
