#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>     // std::lround: correct float-to-byte rounding
#include <cstdio>
#include <cstring>

#include "core/util/ControlSurface.h"   // SurfaceControl: the banks the addresses name

/// @defgroup OscPacket The OSC 1.0 wire format
/// @{
/// The one place the layout lives, with the constants and an inline parse.
///
/// @moreinfo
///
/// This follows the same convention the Art-Net and audio-sync packets do, pinned by a golden byte vector so it cannot drift from the spec.
/// The OSC module receives with it, and nothing sends yet.
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
/// Two type tags are read, a 32-bit integer and a 32-bit float.
/// The spec also has strings, blobs and four tags carrying no payload at all; a control value is a number, so the rest are skipped rather than rejected.
/// A controller that appends a label must not make the whole message unusable.
///
/// ## What each value form means
///
/// Applications overwhelmingly send a float between zero and one, while hardware bridges usually send an integer in the target's own range.
/// Both are accepted and clamped rather than rejected.
/// A controller sending a narrower range must not appear dead, and a float slightly past one is a rounding artifact rather than an error.
///
/// The boolean reading is separate from the byte one, because scaling to a byte rounds a small float to zero.
/// A tiny value is not off; it is a controller sending a range where a switch wanted a flag.
///
/// The encoder is deliberately the only one, and emits a float: every value we send is a number, and that is what applications expect.
/// The parser accepts an integer too, because bridges send those, but nothing forces us to emit one.
///
/// ## Parsing is the security surface
///
/// This reads an unauthenticated datagram off the LAN, so every read is length-checked against the buffer end and nothing is allocated.
/// A malformed packet returns false and never reads past the length it was given.
///
/// Bundles are recognized and rejected: controllers send plain messages, and a bundle is a timed batch we have no use for yet.
///
/// One guard needs naming. A padded string length overshoots whenever the terminator sits in the last few bytes of the buffer, so it must be tested against what remains as well as against zero.
/// Subtracting an overshoot from an unsigned remaining count wraps it to a huge number.
/// Every later size guard then passes, and the next argument is read off the end of the datagram.

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

/// @}
} // namespace mm::osc
