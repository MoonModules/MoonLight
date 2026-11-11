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

TEST_CASE("a byte while the client's own bytes are still going out ends the update as out of turn, and bytes after the end change nothing") {
    Client c;
    c.begin(100, kMd5);                   // the magic bytes queued, none sent yet
    c.received(0x00);
    CHECK(c.state() == Client::State::Failed);
    CHECK(std::string(c.error()) == "an answer out of turn");

    Client d;
    d.begin(100, kMd5);
    drain(d);
    d.received(0x00); d.received(0x02); drain(d);
    d.received(0x40); d.received(0x41); drain(d);
    d.received(0x42); drain(d);
    d.received(0x43);
    d.dataSent(100);
    d.received(0x47); d.received(0x44); d.received(0x45); drain(d);
    REQUIRE(d.state() == Client::State::Done);
    for (int b = 0; b < 256; b++) d.received(static_cast<uint8_t>(b));   // a device that keeps talking
    CHECK(d.state() == Client::State::Done);
}

TEST_CASE("the relay's query takes a whole dotted-quad host and a whole 32-digit lowercase MD5, and refuses anything else") {
    using mm::esphomeota::parseRelayQuery;
    char host[16], md5[33];
    uint8_t ip[4];
    REQUIRE(parseRelayQuery("host=192.168.1.227&md5=0123456789abcdef0123456789abcdef", host, ip, md5));
    CHECK(std::string(host) == "192.168.1.227");
    CHECK(ip[3] == 227);
    CHECK(std::string(md5) == kMd5);
    CHECK(parseRelayQuery("md5=0123456789abcdef0123456789abcdef&host=10.0.0.1", host, ip, md5));   // either order
    const char* refused[] = {
        "",                                                                   // nothing
        "host=192.168.1.227",                                                 // no MD5
        "md5=0123456789abcdef0123456789abcdef",                               // no host
        "ghost=192.168.1.227&md5=0123456789abcdef0123456789abcdef",           // another parameter ending in host
        "host=192.168.1.227x&md5=0123456789abcdef0123456789abcdef",           // a host with more after it
        "host=192.168.1.2270&md5=0123456789abcdef0123456789abcdef",           // an octet past 255
        "host=1.2.3&md5=0123456789abcdef0123456789abcdef",                    // three octets
        "host=evil.example&md5=0123456789abcdef0123456789abcdef",             // a name, not an address
        "host=192.168.1.227&md5=0123456789ABCDEF0123456789abcdef",            // uppercase hex
        "host=192.168.1.227&md5=0123456789abcdef0123456789abcde",             // 31 digits
        "host=192.168.1.227&md5=0123456789abcdef0123456789abcdef0",           // 33 digits
        "host=192.168.1.227&md5=0123456789abcdef0123456789abcdef%00",         // trailing bytes
    };
    for (const char* q : refused) CHECK_MESSAGE(!parseRelayQuery(q, host, ip, md5), q);
}
