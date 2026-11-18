#pragma once

#include <cctype>   // std::isalpha: a letter makes an entry a name
#include <cstdint>
#include <cstdlib>  // std::strtol
#include <cstring>  // std::memcpy

namespace mm {

/// One human-typed text control turned into the receivers a service sends to: addresses, names, or both.
///
/// @technical
///
/// ## Why a list rather than one receiver per service
///
/// Art-Net 4 unicasts ArtDmx to the node owning each universe, so several receivers mean several entries; one field holds a row of tubes on consecutive addresses, as `parsePinList` holds a list of GPIOs.
///
/// ## The syntaxes, which mix freely
///
/// | Written | What it means |
/// |---------|---------------|
/// | `192.168.1.60-70` | a range over the last octet alone, giving `.60` through `.70` |
/// | `192.168.1.60,61,62,65` | after one full dotted quad, a bare number is the next receiver on the same `/24` |
/// | `192.168.1.60, 10.0.0.5` | explicit full quads, across any subnets |
/// | `panel-01.local, panel-02.lan` | names, resolved in the background: `.local` by mDNS, anything else by DNS |
/// | `Stage` | a device group's name: each of its members at its current address, this device left out |
///
/// An entry with a letter in it is a name, and anything else must parse as numbers, so a mistyped address is refused rather than taken for a name.
///
/// ## Parsed, then resolved
///
/// The parser reads the syntax and returns null, or a static error literal the caller hands straight to `setStatus`; a name is kept as its place in the text, so 32 receivers cost 224 bytes.
///
/// `resolveReceivers` in NameResolver.h then turns a name into its address and a group's name into its members.

/// The most receivers one list holds, every service's the same: a device group of up to 32 members, or a wall of tubes, not a subnet scan.
inline constexpr uint8_t kMaxReceivers = 32;

/// One receiver: a literal address, a name and the address it last resolved to, or a device group's member: @xref{parsed-then-resolved}.
struct Receiver {
    uint8_t ip[4] = {};    ///< the address; for a name, 0.0.0.0 until it first resolves, and for a member while it is unheard
    uint8_t nameAt = 0;    ///< where the name starts in the list's text
    uint8_t nameLen = 0;   ///< the name's length, 0 for a literal address
    bool    member = false;   ///< one of a device group's members, which a group's name in the list expanded to
    /// Whether this receiver is a name to resolve.
    bool isName() const { return nameLen != 0; }
    /// Whether this receiver has an address to send to.
    bool hasAddress() const { return ip[0] || ip[1] || ip[2] || ip[3]; }
};

/// The longest name an entry may carry, a DNS label's own limit.
inline constexpr uint8_t kMaxHostName = 63;

/// Copy a receiver's name out of the list's text, NUL-terminated, into `out` of at least kMaxHostName + 1 bytes.
inline void receiverName(const char* text, const Receiver& r, char* out) {
    std::memcpy(out, text + r.nameAt, r.nameLen);
    out[r.nameLen] = 0;
}

namespace detail {
/// Parse one quad, or a bare last-octet shorthand continuing `prev`, advancing `p` past the token.
inline bool parseOneIp(const char*& p, const uint8_t prev[4], bool havePrev, uint8_t out[4]) {
    long o[4];
    char* end = nullptr;
    o[0] = std::strtol(p, &end, 10);
    if (end == p || o[0] < 0 || o[0] > 255) return false;
    const char* q = end;
    uint8_t n = 1;
    while (n < 4 && *q == '.') {
        const char* after = q + 1;
        char* e2 = nullptr;
        o[n] = std::strtol(after, &e2, 10);
        if (e2 == after || o[n] < 0 || o[n] > 255) return false;
        q = e2;
        n++;
    }
    if (n == 4) {                       // a full dotted quad
        for (uint8_t i = 0; i < 4; i++) out[i] = static_cast<uint8_t>(o[i]);
    } else if (n == 1 && havePrev) {    // a bare receiver number → same /24 as the previous address
        std::memcpy(out, prev, 3);
        out[3] = static_cast<uint8_t>(o[0]);
    } else {
        return false;                   // a bare number with no previous address to extend
    }
    p = q;
    return true;
}

/// Whether a character belongs in a receiver name.
inline bool isNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_';
}

/// Whether the entry at `p` contains a letter before its comma, which makes it a name.
inline bool entryIsName(const char* p) {
    for (; *p && *p != ',' && *p != ' '; p++)
        if (std::isalpha(static_cast<unsigned char>(*p))) return true;
    return false;
}
}  // namespace detail

/// Parse `s` into `out`, `nOut` receiving the count, a blank string yielding none.
inline const char* parseReceiverList(const char* s, Receiver* out, uint8_t maxReceivers, uint8_t& nOut) {
    nOut = 0;
    if (!s) return nullptr;
    const char* p = s;
    while (*p == ' ') p++;
    if (!*p) return nullptr;            // blank → no receivers, caller idles

    uint8_t prev[4] = {};
    bool havePrev = false;

    while (true) {
        if (detail::entryIsName(p)) {
            const char* start = p;
            while (detail::isNameChar(*p)) p++;
            const size_t len = static_cast<size_t>(p - start);
            if (len == 0 || len > kMaxHostName || start[0] == '-' || start[0] == '.') return "invalid receiver name";
            if (static_cast<size_t>(start - s) > 255) return "receiver list too long";
            if (nOut >= maxReceivers) return "too many receivers";
            out[nOut] = Receiver{};
            out[nOut].nameAt = static_cast<uint8_t>(start - s);
            out[nOut].nameLen = static_cast<uint8_t>(len);
            nOut++;
        } else {
            uint8_t ip[4];
            if (!detail::parseOneIp(p, prev, havePrev, ip)) return "invalid receiver list";
            while (*p == ' ') p++;
            uint8_t last = ip[3];
            if (*p == '-') {                // a RANGE: expand low..high over the last octet
                p++;
                while (*p == ' ') p++;
                char* end = nullptr;
                const long hi = std::strtol(p, &end, 10);
                if (end == p || hi < 0 || hi > 255) return "invalid ip range";
                if (hi < ip[3]) return "ip range runs backwards";
                last = static_cast<uint8_t>(hi);
                p = end;
            }
            for (int o = ip[3]; o <= last; o++) {
                if (nOut >= maxReceivers) return "too many receivers";
                out[nOut] = Receiver{};
                std::memcpy(out[nOut].ip, ip, 3);
                out[nOut].ip[3] = static_cast<uint8_t>(o);
                nOut++;
            }
            std::memcpy(prev, ip, 4);
            havePrev = true;
        }

        while (*p == ' ') p++;
        if (*p == '\0') return nullptr;
        if (*p != ',') return "invalid receiver list";
        p++;
        while (*p == ' ') p++;
    }
}

}  // namespace mm
