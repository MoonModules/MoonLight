/// @module platform

/// A desktop's own address is its LAN's: the default route's when a LAN interface holds it, and the broadcasting interface's when a VPN holds the route.

#include "doctest.h"
#include "platform/desktop/host_address.h"

#ifndef _WIN32

using mm::platform::HeldAddress;
using mm::platform::lanAddress;

namespace {
constexpr unsigned kEthernet = IFF_UP | IFF_RUNNING | IFF_BROADCAST;
constexpr unsigned kTunnel = IFF_UP | IFF_RUNNING | IFF_POINTOPOINT;   // WireGuard, OpenVPN's tun, macOS's utun
constexpr unsigned kLoopback = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
constexpr uint32_t kLan = 0x0A01A8C0;   // 192.168.1.10 in network order
constexpr uint32_t kVpn = 0x0240400A;   // 10.64.64.2
constexpr uint32_t kHome = 0x0100007F;  // 127.0.0.1
}  // namespace

TEST_CASE("the default route's address is the host's when a LAN interface holds it") {
    const HeldAddress held[] = {{kHome, kLoopback, true}, {kLan, kEthernet, false}};
    CHECK(lanAddress(kLan, held, 2) == kLan);
}

TEST_CASE("a VPN holding the default route leaves the host on its LAN address, split tunnel or not") {
    const HeldAddress held[] = {{kHome, kLoopback, true}, {kLan, kEthernet, false}, {kVpn, kTunnel, false}};
    CHECK(lanAddress(kVpn, held, 3) == kLan);
}

TEST_CASE("a container's bridge, which broadcasts like Ethernet, is passed over whatever order the host lists it in") {
    constexpr uint32_t kDocker = 0x010011AC;   // 172.17.0.1
    const HeldAddress held[] = {{kDocker, kEthernet, true}, {kLan, kEthernet, false}, {kVpn, kTunnel, false}};
    CHECK(lanAddress(kVpn, held, 3) == kLan);
}

TEST_CASE("the virtual interfaces are named by family: a macOS tunnel with its number, a Linux container network with any suffix") {
    using mm::platform::virtualInterface;
    for (const char* name : {"lo", "lo0", "utun3", "awdl0", "bridge100", "docker0", "veth1a2b3c", "br-4f2e9c", "virbr0"})
        CHECK_MESSAGE(virtualInterface(name), name);
    for (const char* name : {"en0", "eth0", "enp3s0", "wlan0", "wlp2s0", "apple", "local"})
        CHECK_FALSE_MESSAGE(virtualInterface(name), name);
}

TEST_CASE("an interface that is down or without carrier is passed over for one that runs") {
    constexpr uint32_t kCableOut = 0x0B01A8C0;   // 192.168.1.11
    const HeldAddress held[] = {{kCableOut, IFF_UP | IFF_BROADCAST, false}, {kLan, kEthernet, false}, {kVpn, kTunnel, false}};
    CHECK(lanAddress(kVpn, held, 3) == kLan);
}

TEST_CASE("a host whose only network is the tunnel keeps the route's address, and a host with no route keeps its LAN's") {
    const HeldAddress tunnelOnly[] = {{kHome, kLoopback, true}, {kVpn, kTunnel, false}};
    CHECK(lanAddress(kVpn, tunnelOnly, 2) == kVpn);
    const HeldAddress lanOnly[] = {{kLan, kEthernet, false}};
    CHECK(lanAddress(0, lanOnly, 1) == kLan);
    CHECK(lanAddress(0, nullptr, 0) == 0);
}

#endif
