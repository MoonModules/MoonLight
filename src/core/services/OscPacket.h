#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>     // std::lround: correct float-to-byte rounding
#include <cstdio>
#include <cstring>

#include "core/util/ControlSurface.h"   // SurfaceControl: the banks the addresses name

/// The one place the layout lives, with the constants and an inline parse.
///
/// @technical
///
/// It follows the convention of the Art-Net and audio-sync packets, pinned by a golden byte vector so it holds to the spec.
///
/// The OSC module receives and sends its feedback with it; a device group's definition travels as one of its messages, written by Writer and read by Reader.
///
/// ## The message layout
///
/// A message is three parts, each padded to a four-byte boundary, everything big-endian.
///
/// | Part | What it holds |
/// |------|---------------|
/// | address pattern | a string starting with a slash, NUL-terminated, then padded to a multiple of four |
/// | type tag string | a string starting with a comma, one character per argument |
/// | arguments | in tag order, each aligned to four bytes |
///
/// A control message reads two type tags, a 32-bit integer and a 32-bit float; Reader reads strings and 64-bit integers too, for a message of several arguments.
///
/// A control value is a number, so a control message skips the spec's other tags (strings, blobs, the four with no payload): a controller that appends a label keeps its message usable.
///
/// ## What each value form means
///
/// Applications mostly send a float between zero and one, hardware bridges an integer in the target's range; both are accepted and clamped, so a narrower range still moves the control and a float past one rounds.
///
/// The boolean reading is apart from the byte one, since scaling to a byte rounds a small float to zero: a tiny value is a controller sending a range where a switch wanted a flag, and it reads as on.
///
/// A control's message goes out as a float, what applications expect; the parser takes an integer too, because bridges send those.
///
/// ## Parsing is the security surface
///
/// It reads an unauthenticated datagram off the LAN, so every read is length-checked against the buffer end, works in the caller's buffer alone, and a malformed packet returns false having read only its own length.
///
/// A controller's bundle is recognized and refused: controllers send plain messages. A device group's change travels as a bundle of one message, its timetag the fleet time it applies at (readBundle, writeBundle).
///
/// One guard needs naming: a padded string length overshoots when the terminator sits in the buffer's last bytes, so it is tested against what remains as well as against zero.
///
/// An overshoot subtracted from an unsigned remaining count wraps it to a huge number, every later size guard then passes, and the next argument is read off the end of the datagram.

namespace mm::osc {

/// The de-facto OSC receive port.
inline constexpr uint16_t kDefaultPort = 9000;
/// Where a board sends its feedback by default, the port a controller listens on.
inline constexpr uint16_t kDefaultFeedbackPort = 9001;
/// The multicast group feedback goes to by default, beside discovery's 239.255.77.77.
inline constexpr char kDefaultGroup[] = "239.255.77.78";

/// One parsed message: the address, and the first numeric argument as both forms: @xref{what-each-value-form-means}.
struct Message {
    const char* address = nullptr;   ///< points INTO the caller's buffer, NUL-terminated there
    float    f = 0.0f;               ///< first numeric argument as a float ('i' converted)
    int32_t  i = 0;                  ///< first numeric argument as an int ('f' truncated)
    bool     hasValue = false;       ///< false when the message carried no numeric argument
    bool     wasFloat = false;       ///< which form arrived, so a caller can scale 0..1 correctly
};

/// Round `n` up to the next multiple of 4, the padding every OSC element uses: @xref{the-message-layout}.
inline constexpr size_t pad4(size_t n) { return (n + 3u) & ~static_cast<size_t>(3u); }

/// Big-endian loads: OSC is network byte order, which is not the host's on any target we build.
inline int32_t beInt32(const uint8_t* p) {
    return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8) |
                                 static_cast<uint32_t>(p[3]));
}
inline float beFloat32(const uint8_t* p) {
    const int32_t bits = beInt32(p);
    float f;
    std::memcpy(&f, &bits, 4);   // the standard type-pun; a reinterpret_cast here is UB
    return f;
}

/// The padded length of the string at `p`, or 0 when it is not terminated inside the buffer.
inline size_t stringLen(const uint8_t* p, size_t avail) {
    for (size_t k = 0; k < avail; k++)
        if (p[k] == '\0') return pad4(k + 1);
    return 0;   // ran off the end without a terminator
}

/// Parse one message, false for anything unusable, having read nothing past `len`: @xref{parsing-is-the-security-surface}.
inline bool parse(const uint8_t* pkt, size_t len, Message& out) {
    if (!pkt || len < 8) return false;                    // shorter than the smallest valid message
    if (pkt[0] == '#') return false;                      // "#bundle": not handled, see the header
    if (pkt[0] != '/') return false;                      // an address pattern always starts with '/'

    const size_t addrLen = stringLen(pkt, len);
    if (addrLen == 0 || addrLen >= len) return false;     // unterminated, or nothing after it
    out.address = reinterpret_cast<const char*>(pkt);

    const uint8_t* tags = pkt + addrLen;
    const size_t tagsAvail = len - addrLen;
    if (tags[0] != ',') return false;                     // the type tag string always starts with ','
    const size_t tagsLen = stringLen(tags, tagsAvail);
    if (tagsLen == 0) return false;

    const uint8_t* arg = tags + tagsLen;
    size_t argAvail = (tagsLen < tagsAvail) ? tagsAvail - tagsLen : 0;

    // A message with no numeric argument is still valid, so the value flag stays false. The bound is tested before the byte and clamped: pad4 rounds a tag string up past the end, which is also why argAvail is a guarded subtraction.
    const size_t tagsBound = (tagsLen < tagsAvail) ? tagsLen : tagsAvail;
    for (size_t t = 1; t < tagsBound && tags[t] != '\0'; t++) {
        switch (tags[t]) {
            case 'i':
                if (argAvail < 4) return false;
                out.i = beInt32(arg);
                out.f = static_cast<float>(out.i);
                out.hasValue = true; out.wasFloat = false;
                return true;
            case 'f':
                if (argAvail < 4) return false;
                out.f = beFloat32(arg);
                out.i = static_cast<int32_t>(out.f);
                out.hasValue = true; out.wasFloat = true;
                return true;
            // Skipped types: step over their payload and keep looking for a number.
            case 's': {
                const size_t n = stringLen(arg, argAvail);
                // Both tests matter: a padded length can exceed what is left. See the appendix.
                if (n == 0 || n > argAvail) return false;
                arg += n; argAvail -= n;
                break;
            }
            case 'b': {
                if (argAvail < 4) return false;
                const int32_t blob = beInt32(arg);
                if (blob < 0) return false;
                const size_t n = 4 + pad4(static_cast<size_t>(blob));
                if (n > argAvail) return false;
                arg += n; argAvail -= n;
                break;
            }
            case 'h': case 'd': case 't':                 // 64-bit types: skip the payload
                if (argAvail < 8) return false;
                arg += 8; argAvail -= 8;
                break;
            case 'T': case 'F': case 'N': case 'I':       // no payload
                break;
            default:
                return false;                             // an unknown tag: we cannot find the args
        }
    }
    return true;   // a valid message that simply carries no number
}

/// Write one message carrying a single 32-bit argument of type `tag`, returning its length or 0 when it will not fit.
inline size_t encodeWord(uint8_t* out, size_t cap, const char* address, char tag, uint32_t bits) {
    if (!out || !address) return 0;
    const size_t addrLen = std::strlen(address);
    if (addrLen == 0 || address[0] != '/') return 0;      // not an address pattern
    const size_t addrPad = pad4(addrLen + 1);
    const size_t tagsPad = pad4(3);                       // "," + the tag + NUL
    const size_t total = addrPad + tagsPad + 4;
    if (total > cap) return 0;

    std::memset(out, 0, total);                           // the padding IS NULs, the address's terminator among them
    std::memcpy(out, address, addrLen);
    out[addrPad] = ',';
    out[addrPad + 1] = tag;

    // Big-endian, the network order parse() reads.
    uint8_t* arg = out + addrPad + tagsPad;
    arg[0] = static_cast<uint8_t>((bits >> 24) & 0xFF);
    arg[1] = static_cast<uint8_t>((bits >> 16) & 0xFF);
    arg[2] = static_cast<uint8_t>((bits >> 8) & 0xFF);
    arg[3] = static_cast<uint8_t>(bits & 0xFF);
    return total;
}

/// Write one message carrying a single float, returning its length or 0 when it will not fit.
inline size_t encodeFloat(uint8_t* out, size_t cap, const char* address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);                        // the type pun, as beFloat32 does
    return encodeWord(out, cap, address, 'f', bits);
}

/// The address word of each surface bank, as `/mm/<bank>/<n>` names one control; a pad's state has its own, apart from the press.
inline const char* surfaceBank(SurfaceControl kind) {
    return kind == SurfaceControl::Switch ? "switch" : kind == SurfaceControl::Encoder ? "encoder"
         : kind == SurfaceControl::Fader ? "fader" : "padstate";
}

/// Write the message for one surface control, `index` zero-based: a fraction of the byte range, or a pad's state as an int.
inline size_t encodeSurface(uint8_t* out, size_t cap, SurfaceControl kind, uint8_t index, uint8_t value) {
    char addr[24];
    std::snprintf(addr, sizeof(addr), "/mm/%s/%u", surfaceBank(kind), static_cast<unsigned>(index) + 1u);
    if (kind == SurfaceControl::Pad) return encodeWord(out, cap, addr, 'i', value);   // empty, stored or applied, not a fraction
    return encodeFloat(out, cap, addr, static_cast<float>(value) / 255.0f);
}

/// Whether this message means on, for a boolean destination where any nonzero value does.
inline bool isTruthy(const Message& m) {
    if (!m.hasValue) return false;
    return m.wasFloat ? (m.f != 0.0f) : (m.i != 0);
}

/// The control value a message means, on the byte range.
inline uint8_t toByte(const Message& m) {
    if (!m.hasValue) return 0;
    if (m.wasFloat) {
        const float v = m.f <= 0.0f ? 0.0f : (m.f >= 1.0f ? 1.0f : m.f);
        return static_cast<uint8_t>(std::lround(v * 255.0f));
    }
    return static_cast<uint8_t>(m.i <= 0 ? 0 : (m.i >= 255 ? 255 : m.i));
}

/// A bundle's timetag for fleet time `us`: OSC 1.0's NTP format, seconds in the high word and the fraction of a second in the low; the fleet clock has no wall time, so the seconds count fleet time.
inline uint64_t timetagOfUs(int64_t us) {
    const uint64_t u = us > 0 ? static_cast<uint64_t>(us) : 0;
    return ((u / 1000000) << 32) | (((u % 1000000) << 32) / 1000000);
}

/// The fleet time in microseconds a timetag names.
inline int64_t usOfTimetag(uint64_t t) {
    return static_cast<int64_t>((t >> 32) * 1000000 + (((t & 0xFFFFFFFFu) * 1000000) >> 32));
}

/// Wrap one message in a bundle stamped `timetag` into `out`: "#bundle", the timetag, the message's size and the message; 0 when it does not fit.
inline size_t writeBundle(uint8_t* out, size_t cap, uint64_t timetag, const uint8_t* msg, size_t len) {
    if (!len || len % 4 || cap < 20 + len) return 0;
    std::memcpy(out, "#bundle\0", 8);
    for (int b = 0; b < 8; b++) out[8 + b] = static_cast<uint8_t>(timetag >> (56 - 8 * b));
    for (int b = 0; b < 4; b++) out[16 + b] = static_cast<uint8_t>(len >> (24 - 8 * b));
    std::memcpy(out + 20, msg, len);
    return 20 + len;
}

/// The timetag and first message of the bundle in `pkt`; false when `pkt` is no bundle or its message runs past `len`.
inline bool readBundle(const uint8_t* pkt, size_t len, uint64_t& timetag, const uint8_t*& msg, size_t& msgLen) {
    if (!pkt || len < 24 || std::memcmp(pkt, "#bundle\0", 8) != 0) return false;
    timetag = 0;
    for (int b = 0; b < 8; b++) timetag = (timetag << 8) | pkt[8 + b];
    const int32_t n = beInt32(pkt + 16);
    if (n <= 0 || static_cast<size_t>(n) > len - 20) return false;
    msg = pkt + 20;
    msgLen = static_cast<size_t>(n);
    return true;
}

/// A message of several arguments, strings and 32- and 64-bit integers (OSC 1.0's `s`, `i` and `h`), its type tags given up front; `size()` is 0 once anything did not fit.
class Writer {
public:
    /// Begin a message to `address` whose arguments follow `tags`, such as ",shi".
    Writer(uint8_t* out, size_t cap, const char* address, const char* tags) : out_(out), cap_(cap) {
        str(address);
        str(tags);
    }
    /// A string argument.
    void str(const char* s) {
        const size_t n = std::strlen(s), padded = pad4(n + 1);
        if (!room(padded)) return;
        std::memset(out_ + len_, 0, padded);
        std::memcpy(out_ + len_, s, n);
        len_ += padded;
    }
    /// A 32-bit integer argument.
    void i32(uint32_t v) { word(v); }
    /// A 64-bit integer argument, high word first.
    void i64(uint64_t v) { word(static_cast<uint32_t>(v >> 32)); word(static_cast<uint32_t>(v)); }
    /// The message's length, or 0 when it overflowed its buffer.
    size_t size() const { return ok_ ? len_ : 0; }

private:
    bool room(size_t n) { if (!ok_ || len_ + n > cap_) ok_ = false; return ok_; }
    void word(uint32_t v) {
        if (!room(4)) return;
        for (int b = 0; b < 4; b++) out_[len_ + b] = static_cast<uint8_t>(v >> (24 - 8 * b));
        len_ += 4;
    }
    uint8_t* out_;
    size_t cap_, len_ = 0;
    bool ok_ = true;
};

/// The arguments of one message, read in order against its type tags, every read length-checked: @xref{parsing-is-the-security-surface}.
class Reader {
public:
    /// Open the message in `pkt`; `ok()` is false unless it addresses `address` and its tags are readable.
    Reader(const uint8_t* pkt, size_t len, const char* address) : p_(pkt), end_(pkt + len) {
        const size_t a = stringLen(p_, len);
        if (!a || a > len || std::strcmp(reinterpret_cast<const char*>(p_), address) != 0) { ok_ = false; return; }
        p_ += a;
        const size_t t = stringLen(p_, remaining());
        if (!t || t > remaining() || p_[0] != ',') { ok_ = false; return; }   // a string's padding past the end is a truncated datagram
        tags_ = reinterpret_cast<const char*>(p_) + 1;
        p_ += t;
    }
    /// Whether every read so far found its tag and its bytes.
    bool ok() const { return ok_; }
    /// Whether another argument follows.
    bool more() const { return ok_ && *tags_; }
    /// The next argument as a string, or "" when it is none.
    const char* str() {
        if (!take('s')) return "";
        const size_t n = stringLen(p_, remaining());
        if (!n || n > remaining()) { ok_ = false; return ""; }
        const char* s = reinterpret_cast<const char*>(p_);
        p_ += n;
        return s;
    }
    /// The next argument as a 32-bit integer, or 0 when it is none.
    uint32_t i32() { return take('i') && fits(4) ? word() : 0; }
    /// The next argument as a 64-bit integer, or 0 when it is none.
    uint64_t i64() {
        if (!take('h') || !fits(8)) return 0;
        const uint64_t hi = word();
        return (hi << 32) | word();
    }

private:
    bool take(char tag) { if (!ok_ || *tags_ != tag) { ok_ = false; return false; } tags_++; return true; }
    size_t remaining() const { return static_cast<size_t>(end_ - p_); }
    bool fits(size_t n) { if (remaining() < n) ok_ = false; return ok_; }
    uint32_t word() { const uint32_t v = static_cast<uint32_t>(beInt32(p_)); p_ += 4; return v; }
    const uint8_t* p_;
    const uint8_t* end_;
    const char* tags_ = "";
    bool ok_ = true;
};

} // namespace mm::osc
