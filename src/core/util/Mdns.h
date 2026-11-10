#pragma once

/// A DNS-SD browse over multicast DNS (RFC 6762, RFC 6763): the one question Discovery asks, and the reply it reads, for devices that announce nothing else, ESPHome's among them.
///
/// @technical
///
/// **A legacy unicast query.** The question goes to 224.0.0.251:5353 from an ordinary port, so a responder answers that port directly (RFC 6762 § 6.7). Discovery binds nothing on 5353, where the platform's own mDNS responder listens, and keeps no query state of its own.
///
/// **One reply, one device.** A responder answers with its own records: the PTR naming its service instance, the TXT carrying its key=value pairs, and its address. The reply's source address is the device's, so no A record is needed.
///
/// Pure functions on byte buffers, so a desktop test reads a captured reply; nothing here allocates.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm::mdns {

inline constexpr uint8_t  kGroup[4] = {224, 0, 0, 251};   ///< the mDNS group (RFC 6762 § 3)
inline constexpr uint16_t kPort = 5353;                     ///< its port
inline constexpr uint16_t kTypePtr = 12, kTypeTxt = 16, kClassIn = 1;

/// What a reply says about one service instance.
struct Service {
    char instance[40] = {};   ///< the instance's own label, "esphome-web-7388c8"
    char name[33] = {};       ///< its TXT friendly_name, or else the instance label
    char version[16] = {};    ///< its TXT version
    char board[24] = {};      ///< its TXT board, the chip's board ("esp32dev")
};

/// Write a query for PTR `service` ("_esphomelib._tcp.local") into `out`; its length, or 0 when it does not fit.
inline size_t buildQuery(const char* service, uint8_t* out, size_t cap) {
    if (cap < 12) return 0;
    std::memset(out, 0, 12);
    out[0] = 0x4d; out[1] = 0x4d;   // an id the reply echoes; any value serves
    out[5] = 1;                     // one question
    size_t at = 12;
    for (const char* label = service; *label;) {
        const char* dot = std::strchr(label, '.');
        const size_t n = dot ? static_cast<size_t>(dot - label) : std::strlen(label);
        if (n == 0 || n > 63 || at + 1 + n + 5 > cap) return 0;
        out[at++] = static_cast<uint8_t>(n);
        std::memcpy(out + at, label, n);
        at += n;
        label += n + (dot ? 1 : 0);
    }
    out[at++] = 0;   // the root
    out[at++] = 0; out[at++] = kTypePtr;
    out[at++] = 0; out[at++] = kClassIn;
    return at;
}

/// Where the compression pointer at `off` leads, or 0 when it is cut off or does not point back: a pointer names an earlier name (RFC 1035 § 4.1.4), so a forward one or a loop is malformed.
inline size_t pointerTarget(const uint8_t* msg, size_t len, size_t off) {
    if (off + 1 >= len) return 0;
    const size_t to = static_cast<size_t>(((msg[off] & 0x3f) << 8) | msg[off + 1]);
    return to < off ? to : 0;
}

/// Append one label to the dotted name in `out`, a dot before every label but the first, counting what does not fit.
inline void appendLabel(char* out, size_t cap, size_t& w, const uint8_t* label, size_t n) {
    if (w) { if (w + 1 < cap) out[w] = '.'; w++; }
    for (size_t i = 0; i < n; i++, w++)
        if (w + 1 < cap) out[w] = static_cast<char>(label[i]);
}

/// Terminate the name in `out` where it fits, answering `next`, the offset after the name.
inline size_t endName(char* out, size_t cap, size_t w, size_t next) {
    if (cap) out[w < cap ? w : cap - 1] = 0;
    return next;
}

/// Read the (possibly compressed) name at `off` as dotted text into `out`; the offset after it in the record, or 0 when malformed.
inline size_t readName(const uint8_t* msg, size_t len, size_t off, char* out, size_t cap) {
    size_t next = 0, w = 0;
    for (int jumps = 0; off < len;) {
        const uint8_t n = msg[off];
        if (n == 0) return endName(out, cap, w, next ? next : off + 1);
        if ((n & 0xc0) == 0xc0) {
            const size_t to = pointerTarget(msg, len, off);
            if (!to || ++jumps > 16) return 0;
            if (!next) next = off + 2;
            off = to;
            continue;
        }
        if (off + 1 + n > len) return 0;
        appendLabel(out, cap, w, msg + off + 1, n);
        off += 1 + n;
    }
    return 0;
}

/// Copy the TXT value of `key` from `rdata` into `out`; false when absent.
inline bool txtValue(const uint8_t* rdata, size_t len, const char* key, char* out, size_t cap) {
    if (cap == 0) return false;
    const size_t k = std::strlen(key);
    for (size_t at = 0; at < len;) {
        const size_t n = rdata[at++];
        if (at + n > len) return false;
        if (n > k && rdata[at + k] == '=' && std::memcmp(rdata + at, key, k) == 0) {
            const size_t v = n - k - 1 < cap - 1 ? n - k - 1 : cap - 1;
            std::memcpy(out, rdata + at + k + 1, v);
            out[v] = 0;
            return true;
        }
        at += n;
    }
    return false;
}

/// The offset after `count` questions starting at `off`, or 0 when one is malformed.
inline size_t skipQuestions(const uint8_t* msg, size_t len, size_t off, size_t count) {
    char name[96];
    for (size_t i = 0; i < count && off; i++) {
        off = readName(msg, len, off, name, sizeof(name));
        off = (off && off + 4 <= len) ? off + 4 : 0;
    }
    return off;
}

/// Hand each of `count` resource records from `off` to `fn(name, type, rdata, rdlen)`; false when one is malformed or `fn` refuses it.
template <class Fn>
bool forEachRecord(const uint8_t* msg, size_t len, size_t off, size_t count, Fn&& fn) {
    char name[96];
    for (size_t i = 0; i < count; i++) {
        off = readName(msg, len, off, name, sizeof(name));
        if (!off || off + 10 > len) return false;
        const uint16_t type = static_cast<uint16_t>((msg[off] << 8) | msg[off + 1]);
        const size_t rdlen = (msg[off + 8] << 8) | msg[off + 9];
        const size_t rdata = off + 10;
        if (rdata + rdlen > len || !fn(name, type, rdata, rdlen)) return false;
        off = rdata + rdlen;
    }
    return true;
}

/// The instance a PTR for `service` names, into `instance`; false when none does, or its name does not read or ends outside its own record.
inline bool findInstance(const uint8_t* msg, size_t len, size_t off, size_t rr, const char* service, char* instance, size_t cap) {
    instance[0] = 0;
    const bool ok = forEachRecord(msg, len, off, rr, [&](const char* name, uint16_t type, size_t rdata, size_t rdlen) {
        if (type != kTypePtr || instance[0] || std::strcmp(name, service) != 0) return true;
        const size_t end = readName(msg, len, rdata, instance, cap);
        return end && end <= rdata + rdlen;
    });
    return ok && instance[0];
}

/// The `instance`'s TXT pairs into `out`: its friendly name, version and board.
inline void readTxt(const uint8_t* msg, size_t len, size_t off, size_t rr, const char* instance, Service& out) {
    forEachRecord(msg, len, off, rr, [&](const char* name, uint16_t type, size_t rdata, size_t rdlen) {
        if (type != kTypeTxt || std::strcmp(name, instance) != 0) return true;
        txtValue(msg + rdata, rdlen, "friendly_name", out.name, sizeof(out.name));
        txtValue(msg + rdata, rdlen, "version", out.version, sizeof(out.version));
        txtValue(msg + rdata, rdlen, "board", out.board, sizeof(out.board));
        return true;
    });
}

/// Read a reply to the browse for `service`: the instance its PTR names and that instance's TXT; false when the reply names no instance of it.
inline bool parseReply(const uint8_t* msg, size_t len, const char* service, Service& out) {
    if (len < 12 || !(msg[2] & 0x80)) return false;   // a response, not a question
    const size_t rr = ((msg[6] << 8) | msg[7]) + ((msg[8] << 8) | msg[9]) + ((msg[10] << 8) | msg[11]);
    const size_t off = skipQuestions(msg, len, 12, (msg[4] << 8) | msg[5]);
    if (!off) return false;
    out = Service{};
    char instance[96] = {};
    if (!findInstance(msg, len, off, rr, service, instance, sizeof(instance))) return false;
    readTxt(msg, len, off, rr, instance, out);
    const char* dot = std::strchr(instance, '.');
    const size_t n = dot ? static_cast<size_t>(dot - instance) : std::strlen(instance);
    std::memcpy(out.instance, instance, n < sizeof(out.instance) ? n : sizeof(out.instance) - 1);
    if (!out.name[0]) std::memcpy(out.name, out.instance, sizeof(out.name) - 1);
    return true;
}

}  // namespace mm::mdns
