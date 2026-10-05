/// @module NetworkModule
/// @also EthernetModule, WiFiModule

/// A Static IP setting is used only when it can work on a network, and the card says why when it cannot.

#include "doctest.h"
#include "core/system/IpSettings.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

mm::IpSettings fixed(const char* ip, const char* gateway, const char* subnet, const char* dns = "0.0.0.0") {
    mm::IpSettings s;
    s.mode = mm::ipsettings::kStatic;
    const char* text[4] = {ip, gateway, subnet, dns};
    for (uint8_t k = 0; k < 4; k++) {
        unsigned q[4] = {};
        std::sscanf(text[k], "%u.%u.%u.%u", &q[0], &q[1], &q[2], &q[3]);
        for (int i = 0; i < 4; i++) s.address(k)[i] = static_cast<uint8_t>(q[i]);
    }
    return s;
}

std::string why(const mm::IpSettings& s) { return s.problem() ? s.problem() : ""; }

}  // namespace

TEST_CASE("a Static setting a network can use is used, gateway and DNS optional") {
    CHECK(fixed("192.168.1.9", "192.168.1.1", "255.255.255.0", "1.1.1.1").usable());
    CHECK(fixed("192.168.1.250", "0.0.0.0", "255.255.255.0").usable());   // no gateway, no DNS: a local-only device
    CHECK(fixed("169.254.7.9", "0.0.0.0", "255.255.0.0").usable());       // link-local, a direct cable
    CHECK(fixed("10.0.3.4", "10.0.0.1", "255.255.240.0").usable());       // a mask that is not a whole octet
    CHECK_FALSE(mm::IpSettings{}.usable());                               // DHCP is never a Static setting to use
}

TEST_CASE("a Static setting that cannot work is named, rule by rule") {
    CHECK(why(fixed("192.168.1.9", "0.0.0.0", "255.0.255.0")) == "static IP not used: the subnet mask is not a valid mask");
    CHECK(why(fixed("192.168.1.9", "0.0.0.0", "0.0.0.0")) == "static IP not used: the subnet mask is not a valid mask");
    CHECK(why(fixed("0.0.0.0", "0.0.0.0", "255.255.255.0")) == "static IP not used: no address is set");
    CHECK(why(fixed("127.0.0.5", "0.0.0.0", "255.0.0.0")) == "static IP not used: the address is not a host address");
    CHECK(why(fixed("239.1.1.1", "0.0.0.0", "255.255.255.0")) == "static IP not used: the address is not a host address");
    CHECK(why(fixed("192.168.1.0", "0.0.0.0", "255.255.255.0")) == "static IP not used: the address is the subnet's network or broadcast address");
    CHECK(why(fixed("192.168.1.255", "0.0.0.0", "255.255.255.0")) == "static IP not used: the address is the subnet's network or broadcast address");
    CHECK(why(fixed("192.168.1.9", "192.168.2.1", "255.255.255.0")) == "static IP not used: the gateway is outside the subnet");
    CHECK(why(fixed("192.168.1.9", "192.168.1.9", "255.255.255.0")) == "static IP not used: the gateway is the device's own address");
    CHECK(why(fixed("192.168.1.9", "192.168.1.1", "255.255.255.0", "224.0.0.251")) == "static IP not used: the DNS server is not a host address");
    CHECK(why(fixed("192.168.1.9", "192.168.1.0", "255.255.255.0")) == "static IP not used: the gateway is the subnet's network or broadcast address");
    CHECK(why(fixed("192.168.1.9", "192.168.1.255", "255.255.255.0")) == "static IP not used: the gateway is the subnet's network or broadcast address");
    CHECK_FALSE(fixed("192.168.1.9", "192.168.2.1", "255.255.255.0").usable());
}

// A /31 is a two-address point-to-point link (RFC 3021) with no network or broadcast address, so both ends are hosts.
TEST_CASE("both addresses of a point-to-point /31 are usable") {
    CHECK(fixed("10.0.0.0", "10.0.0.1", "255.255.255.254").usable());
    CHECK(fixed("10.0.0.1", "10.0.0.0", "255.255.255.254").usable());
    CHECK(why(fixed("10.0.0.2", "0.0.0.0", "255.255.255.252")) == "");
    CHECK(why(fixed("10.0.0.3", "0.0.0.0", "255.255.255.252")) == "static IP not used: the address is the subnet's network or broadcast address");
}

// Switching to Static before an address is typed keeps the lease in effect, so the interface is not restarted and the open page stays connected.
TEST_CASE("an edit that leaves the addressing in effect unchanged applies nothing") {
    mm::IpApplied applied;
    applied.mark(mm::IpSettings{});                                            // running on DHCP
    CHECK_FALSE(applied.changedTo(fixed("0.0.0.0", "0.0.0.0", "255.255.255.0")));   // Static, no address yet
    CHECK_FALSE(applied.changedTo(fixed("192.168.1.9", "192.168.2.1", "255.255.255.0")));   // still unusable
    CHECK(applied.changedTo(fixed("192.168.1.9", "192.168.1.1", "255.255.255.0")));   // now in effect
    CHECK(applied.changedTo(mm::IpSettings{}));                                // back to DHCP
}

// Ethernet switched to Static with no cable lease starts from the network the station is on: its gateway, mask and DNS server, the address left to choose.
TEST_CASE("a Static setting can start from another interface's network, its address left to choose") {
    const uint8_t staIp[4] = {192, 168, 8, 158}, gw[4] = {192, 168, 8, 1}, mask[4] = {255, 255, 255, 0};
    mm::platform::setTestWifiStaIPv4(staIp);
    mm::platform::setTestNetLease(gw, mask, nullptr);
    mm::IpSettings s;
    s.mode = mm::ipsettings::kStatic;
    s.prefillNetworkFrom(mm::platform::NetIface::Sta);
    CHECK(std::memcmp(s.ip, "\0\0\0\0", 4) == 0);
    CHECK(std::memcmp(s.gateway, gw, 4) == 0);
    CHECK(std::memcmp(s.subnet, mask, 4) == 0);
    CHECK(std::memcmp(s.dns, gw, 4) == 0);   // the gateway stands in for the DNS server the lease does not name
    CHECK(why(s) == "static IP not used: no address is set");
    mm::platform::setTestWifiStaIPv4(nullptr);
    mm::platform::setTestNetLease(nullptr, nullptr, nullptr);
}
