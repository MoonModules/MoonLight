#pragma once

/// @defgroup CaptivePortal What makes a phone joining the access point open the UI
/// @{
/// The two pieces a captive portal is made of, as pure functions the desktop tests: a DNS reply (RFC 1035) and the HTTP redirect decision.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm::captive {

/// The access point's own address, which every name resolves to while it runs.
inline constexpr uint8_t kAddress[4] = {4, 3, 2, 1};
// The same address as text, spelled once: a macro, since only string-literal concatenation builds the redirect below at compile time.
#define MM_CAPTIVE_ADDRESS "4.3.2.1"
/// The address as the text a Host header and a link carry.
inline constexpr const char* kAddressText = MM_CAPTIVE_ADDRESS;
/// The response every redirected request gets, a `302` to the UI at that address, open on the WiFi card where setup continues.
inline constexpr const char* kRedirect =
    "HTTP/1.1 302 Found\r\nLocation: http://" MM_CAPTIVE_ADDRESS "/?open=WiFi\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
/// The largest DNS message over UDP (RFC 1035 § 2.3.4), and so the receive buffer's size.
inline constexpr size_t kMaxMessage = 512;

/// Turn the query in `msg` into its reply in place, answering an A question with `ip`: the reply's length, or 0 to drop it.
inline size_t dnsReply(uint8_t* msg, size_t len, size_t cap, const uint8_t ip[4]) {
    if (!msg || len < 12 || cap < len) return 0;
    if (msg[2] & 0x80) return 0;                   // a response, not a query
    if ((msg[2] & 0x78) != 0) return 0;            // not a standard query
    if (msg[4] != 0 || msg[5] != 1) return 0;      // exactly one question, as every resolver sends
    // The question's name: labels of 1 to 63 bytes, at most 255 in all, ending at a zero byte inside the packet.
    size_t p = 12;
    while (true) {
        if (p >= len) return 0;
        const uint8_t label = msg[p];
        if (label == 0) { p++; break; }
        if (label > 63 || p - 12 + label + 1 > 255) return 0;   // a compression pointer or an overlong name
        p += 1 + label;
    }
    if (p + 4 > len) return 0;                     // no room for its type and class
    const bool isA = msg[p] == 0 && msg[p + 1] == 1 && msg[p + 2] == 0 && msg[p + 3] == 1;
    p += 4;
    const size_t replyLen = p + (isA ? 16 : 0);
    if (replyLen > cap) return 0;
    msg[2] = 0x84 | (msg[2] & 0x01);               // a response, authoritative, recursion-desired echoed
    msg[3] = 0x00;                                 // no error
    msg[6] = 0; msg[7] = isA ? 1 : 0;              // the answer count
    msg[8] = msg[9] = msg[10] = msg[11] = 0;       // no authority, no additional records (the query's EDNS record is dropped)
    if (isA) {
        static constexpr uint8_t kAnswer[12] = {
            0xC0, 0x0C,               // the name: a pointer to the question's
            0x00, 0x01, 0x00, 0x01,   // type A, class IN
            0x00, 0x00, 0x00, 0x3C,   // 60 seconds, so a phone that leaves asks its own network again soon
            0x00, 0x04,               // four bytes of address
        };
        std::memcpy(msg + p, kAnswer, sizeof(kAnswer));
        std::memcpy(msg + p + sizeof(kAnswer), ip, 4);
    }
    return replyLen;
}

/// Whether a connection reached this device at `localIp`, its access point's own address.
inline bool throughAccessPoint(const uint8_t localIp[4]) { return std::memcmp(localIp, kAddress, 4) == 0; }

// The stack takes a packet for any of the device's addresses on any interface, so only a client's own address says it is on the access point.
/// Whether a client at `peerIp` is on the access point, whose DHCP alone hands out addresses in its /24.
inline bool fromAccessPoint(const uint8_t peerIp[4]) { return std::memcmp(peerIp, kAddress, 3) == 0; }

/// Whether a request at `localIp` for `host` and `path` is sent to the UI: a page asked for through the access point under another name. The API answers as itself.
inline bool redirects(const uint8_t localIp[4], const char* host, const char* path) {
    if (!throughAccessPoint(localIp)) return false;
    if (path && std::strncmp(path, "/api/", 5) == 0) return false;
    if (!host || !host[0]) return false;                        // HTTP/1.0 without a Host: serve it
    const size_t n = std::strlen(kAddressText);
    if (std::strncmp(host, kAddressText, n) == 0 && (host[n] == '\0' || host[n] == ':' || host[n] == '\r')) return false;
    return true;
}

}  // namespace mm::captive

/// @}
