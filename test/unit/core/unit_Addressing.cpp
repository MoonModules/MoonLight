/// @module Addressing

/// One rule for reaching many receivers: each mode sends exactly where it says, and a network proven to drop multicast adds a broadcast copy.

#include "doctest.h"
#include "core/util/Addressing.h"

#include <string>
#include <vector>

namespace {
/// The addresses one send reached, as dotted text.
struct Recorder {
    std::vector<std::string> to;
    void operator()(const uint8_t ip[4]) {
        to.push_back(std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]));
    }
};
mm::Host host(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    mm::Host h;
    h.ip[0] = a; h.ip[1] = b; h.ip[2] = c; h.ip[3] = d;
    return h;
}
const mm::Host kHosts[3] = {host(192, 168, 1, 10), host(0, 0, 0, 0), host(192, 168, 1, 12)};
const uint8_t kGroup[4] = {239, 255, 77, 78};
}  // namespace

TEST_CASE("unicast sends a copy to each host, skipping one not resolved yet") {
    Recorder r;
    mm::sendAddressed(mm::Addressing::Unicast, mm::Traffic::Occasional, kHosts, 3, kGroup, r);
    CHECK(r.to == std::vector<std::string>{"192.168.1.10", "192.168.1.12"});
}

TEST_CASE("multicast sends one copy to the group, and broadcast one to every device") {
    Recorder m;
    mm::sendAddressed(mm::Addressing::Multicast, mm::Traffic::Occasional, kHosts, 3, kGroup, m);
    CHECK(m.to == std::vector<std::string>{"239.255.77.78"});
    Recorder b;
    mm::sendAddressed(mm::Addressing::Broadcast, mm::Traffic::Occasional, kHosts, 3, kGroup, b);
    CHECK(b.to == std::vector<std::string>{"255.255.255.255"});
    Recorder both;
    mm::sendAddressed(mm::Addressing::MulticastBroadcast, mm::Traffic::Occasional, kHosts, 3, kGroup, both);
    CHECK(both.to == std::vector<std::string>{"239.255.77.78", "255.255.255.255"});
}

TEST_CASE("a network proven to drop multicast adds a broadcast copy to occasional traffic, never to a light stream") {
    mm::NetworkPath::multicastDropped = true;
    Recorder occasional, stream;
    mm::sendAddressed(mm::Addressing::Multicast, mm::Traffic::Occasional, kHosts, 3, kGroup, occasional);
    mm::sendAddressed(mm::Addressing::Multicast, mm::Traffic::FrameRate, kHosts, 3, kGroup, stream);
    mm::NetworkPath::multicastDropped = false;
    CHECK(occasional.to == std::vector<std::string>{"239.255.77.78", "255.255.255.255"});
    CHECK(stream.to == std::vector<std::string>{"239.255.77.78"});   // a broadcast copy per universe would flood the LAN
}
