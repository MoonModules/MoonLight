#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm::rtpmidi {

/// @defgroup RtpMidi RTP-MIDI and AppleMIDI packets
/// @{
/// MIDI over a network as RFC 6295 lays it out, with the session packets of Apple's Network MIDI; the session itself is `RtpMidiSession`.
/// A session packet starts with 0xFFFF and two ASCII letters naming it, and any other datagram is an RTP packet carrying MIDI.

/// The control port a session listens on unless told otherwise; data uses the next one up.
inline constexpr uint16_t kDefaultPort = 5004;
/// The protocol version an AppleMIDI session packet carries.
inline constexpr uint32_t kVersion = 2;
/// RTP's payload type for MIDI, as Apple's implementation sends it.
inline constexpr uint8_t kPayloadType = 0x61;

/// The session packets, by their two letters.
enum class Command : uint16_t {
    None = 0,
    Invitation = 0x494E,   ///< `IN`
    Accept = 0x4F4B,       ///< `OK`
    Reject = 0x4E4F,       ///< `NO`
    End = 0x4259,          ///< `BY`
    Sync = 0x434B,         ///< `CK`
    Feedback = 0x5253,     ///< `RS`, the receiver's last sequence number, for trimming a journal
};

/// Write `v` big-endian into `n` bytes at `p`.
inline void putBE(uint8_t* p, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; i--) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}

/// Read `n` bytes at `p` as a big-endian number.
inline uint64_t getBE(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

/// The session packet a datagram is, or None for an RTP data packet or junk.
inline Command commandOf(const uint8_t* p, size_t len) {
    if (len < 4 || p[0] != 0xFF || p[1] != 0xFF) return Command::None;
    return static_cast<Command>(getBE(p + 2, 2));
}

/// An `IN`, `OK`, `NO` or `BY` packet, its name pointing into the datagram.
struct Exchange {
    Command command = Command::None;   ///< which of the four
    uint32_t token = 0;   ///< the initiator's, echoed in the answer
    uint32_t ssrc = 0;    ///< the sender's
    char name[32] = {};   ///< the sender's session name, empty when it sent none
};

/// Write an `IN`, `OK`, `NO` or `BY`: version 2, the token, the sender's SSRC, and a name on all but `BY`; the length, 0 when it does not fit.
inline size_t encodeExchange(uint8_t* out, size_t max, Command command, uint32_t token, uint32_t ssrc, const char* name) {
    const size_t nameLen = name && command != Command::End ? std::strlen(name) + 1 : 0;
    const size_t len = 16 + nameLen;
    if (len > max) return 0;
    out[0] = 0xFF; out[1] = 0xFF;
    putBE(out + 2, static_cast<uint16_t>(command), 2);
    putBE(out + 4, kVersion, 4);
    putBE(out + 8, token, 4);
    putBE(out + 12, ssrc, 4);
    if (nameLen) std::memcpy(out + 16, name, nameLen);
    return len;
}

/// Read an `IN`, `OK`, `NO` or `BY`, a name cut to fit and a missing NUL tolerated; false when it is none of them.
inline bool parseExchange(const uint8_t* p, size_t len, Exchange& out) {
    const Command c = commandOf(p, len);
    if (len < 16 || (c != Command::Invitation && c != Command::Accept && c != Command::Reject && c != Command::End)) return false;
    out = Exchange{};
    out.command = c;
    out.token = static_cast<uint32_t>(getBE(p + 8, 4));
    out.ssrc = static_cast<uint32_t>(getBE(p + 12, 4));
    size_t n = 0;
    for (size_t i = 16; i < len && p[i] && n + 1 < sizeof(out.name); i++) out.name[n++] = static_cast<char>(p[i]);
    return true;
}

/// A `CK` packet, which measures the round trip between the two clocks in three steps.
struct Sync {
    uint32_t ssrc = 0;     ///< the sender's
    uint8_t count = 0;     ///< the step, 0 to 2, which is also how many timestamps are filled after the first
    uint64_t ts[3] = {};   ///< the timestamps, in 100-microsecond units
};

/// The length of a `CK` packet.
inline constexpr size_t kSyncLen = 36;

/// Write a `CK`: the SSRC, the count, three bytes of padding and the three timestamps; the length, 0 when it does not fit.
inline size_t encodeSync(uint8_t* out, size_t max, const Sync& s) {
    if (max < kSyncLen) return 0;
    out[0] = 0xFF; out[1] = 0xFF;
    putBE(out + 2, static_cast<uint16_t>(Command::Sync), 2);
    putBE(out + 4, s.ssrc, 4);
    out[8] = s.count;
    out[9] = out[10] = out[11] = 0;
    for (int i = 0; i < 3; i++) putBE(out + 12 + 8 * i, s.ts[i], 8);
    return kSyncLen;
}

/// Read a `CK`; false when it is not one.
inline bool parseSync(const uint8_t* p, size_t len, Sync& out) {
    if (len < kSyncLen || commandOf(p, len) != Command::Sync) return false;
    out.ssrc = static_cast<uint32_t>(getBE(p + 4, 4));
    out.count = p[8];
    for (int i = 0; i < 3; i++) out.ts[i] = getBE(p + 12 + 8 * i, 8);
    return true;
}

/// The bytes of a MIDI message with this status, counted from it: 0 for SysEx, which runs to its end byte.
inline uint8_t messageLength(uint8_t status) {
    if (status < 0xF0) return (status & 0xE0) == 0xC0 ? 2 : 3;   // program change and channel pressure carry one data byte
    switch (status) {
        case 0xF0: return 0;
        case 0xF1: case 0xF3: return 2;
        case 0xF2: return 3;
        default: return 1;   // tune request, the real-time bytes, and the undefined ones
    }
}

/// Builds one data packet, a message at a time: a 12-byte RTP header, a two-byte command section header, then the MIDI list.
/// The list carries no journal, and every message after the first follows a zero delta time.
class DataWriter {
public:
    /// Start a packet in `out` of `max` bytes.
    DataWriter(uint8_t* out, size_t max) : out_(out), max_(max) {}

    /// Add one complete message; false when it does not fit, the packet staying as it was.
    bool add(const uint8_t* msg, size_t len) {
        const size_t need = (count_ ? 1 : 0) + len;
        if (len == 0 || kListAt + list_ + need > max_ || list_ + need > 0x0FFF) return false;
        uint8_t* p = out_ + kListAt + list_;
        if (count_) *p++ = 0x00;   // a zero delta time: every message is sent now
        std::memcpy(p, msg, len);
        list_ += need;
        count_++;
        return true;
    }

    /// How many messages the packet holds.
    size_t count() const { return count_; }

    /// Write the header for sequence number `seq` at `timestamp` from `ssrc`; the packet's length, 0 when it is empty.
    size_t finish(uint16_t seq, uint32_t timestamp, uint32_t ssrc) {
        if (!count_) return 0;
        out_[0] = 0x80;   // version 2, no padding, no extension, no contributing sources
        out_[1] = kPayloadType;
        putBE(out_ + 2, seq, 2);
        putBE(out_ + 4, timestamp, 4);
        putBE(out_ + 8, ssrc, 4);
        // B: the length over two bytes, so a long list needs no other layout.
        out_[12] = static_cast<uint8_t>(0x80 | (list_ >> 8));
        out_[13] = static_cast<uint8_t>(list_);
        return kListAt + list_;
    }

private:
    static constexpr size_t kListAt = 14;   ///< the RTP header, then the two-byte command section header
    uint8_t* out_;
    size_t max_;
    size_t list_ = 0;
    size_t count_ = 0;
};

/// Where a data packet's MIDI list starts and ends, past the RTP header, with the command section's flags; false when it is no data packet.
inline bool findList(const uint8_t* p, size_t len, size_t& at, size_t& end, uint8_t& flags) {
    if (len < 13 || (p[0] >> 6) != 2) return false;
    at = 12 + 4u * (p[0] & 0x0F);   // past any contributing sources
    if (p[0] & 0x10) {              // a header extension: its length in words after a two-byte profile
        if (at + 4 > len) return false;
        at += 4 + 4u * static_cast<size_t>(getBE(p + at + 2, 2));
    }
    if (at >= len) return false;
    // B: a 12-bit length over two bytes; J: a journal after the list; Z: a delta time before the first command.
    flags = p[at];
    if ((flags & 0x80) && at + 1 >= len) return false;
    const size_t listLen = flags & 0x80 ? (static_cast<size_t>(flags & 0x0F) << 8) | p[at + 1] : flags & 0x0Fu;
    at += flags & 0x80 ? 2 : 1;
    end = at + listLen <= len ? at + listLen : len;   // a journal, if any, lies past the list
    return true;
}

/// One MIDI list, read a message at a time, with delta times and running status.
class ListReader {
public:
    /// Read `p` from `at` to `end`, a delta time before the first command when `deltaFirst`.
    ListReader(const uint8_t* p, size_t at, size_t end, bool deltaFirst) : p_(p), i_(at), end_(end), delta_(deltaFirst) {}

    /// The next complete message, false at the end of the list or at bytes that cannot be read.
    bool next(const uint8_t*& msg, size_t& len) {
        while (i_ < end_) {
            if (delta_) skipDelta();
            delta_ = true;   // every command after the first has one
            if (i_ >= end_) return false;
            const uint8_t status = p_[i_] & 0x80 ? p_[i_] : running_;
            if (!status) return false;   // data with no status before it
            if (status != 0xF0 && status != 0xF7) return command(status, msg, len);
            if (sysex(msg, len)) return true;
        }
        return false;
    }

private:
    void skipDelta() {
        for (int b = 0; b < 4 && i_ < end_; b++)
            if (!(p_[i_++] & 0x80)) return;
    }

    /// A channel or system message, its status byte possibly left out.
    bool command(uint8_t status, const uint8_t*& msg, size_t& len) {
        const size_t total = messageLength(status);
        if (p_[i_] & 0x80) i_++;
        if (i_ + total - 1 > end_) { i_ = end_; return false; }
        buf_[0] = status;
        for (size_t k = 1; k < total; k++) buf_[k] = p_[i_++];
        if (status < 0xF0) running_ = status;
        else if (status < 0xF8) running_ = 0;   // a system common message ends running status; real time leaves it
        msg = buf_;
        len = total;
        return true;
    }

    /// A SysEx: true when complete, from F0 to F7; a piece, F0 at its end or F7 at its start, is passed over.
    bool sysex(const uint8_t*& msg, size_t& len) {
        const size_t from = i_++;
        while (i_ < end_ && p_[i_] != 0xF7 && p_[i_] != 0xF0 && p_[i_] != 0xF4) i_++;
        running_ = 0;
        if (i_ >= end_) return false;
        const bool complete = p_[from] == 0xF0 && p_[i_] == 0xF7;
        i_++;
        msg = p_ + from;
        len = i_ - from;
        return complete;
    }

    const uint8_t* p_;
    size_t i_;
    size_t end_;
    bool delta_;
    uint8_t running_ = 0;
    uint8_t buf_[3] = {};
};

/// Call `f(msg, len)` for each complete message in a data packet, a SysEx split over packets skipped; the SSRC into `ssrc`, false for no data packet.
template <class F>
bool forEachMessage(const uint8_t* p, size_t len, uint32_t& ssrc, F&& f) {
    size_t at = 0, end = 0;
    uint8_t flags = 0;
    if (!findList(p, len, at, end, flags)) return false;
    ssrc = static_cast<uint32_t>(getBE(p + 8, 4));
    ListReader reader(p, at, end, flags & 0x20);
    const uint8_t* msg = nullptr;
    size_t n = 0;
    while (reader.next(msg, n)) f(msg, n);
    return true;
}

/// @}
}  // namespace mm::rtpmidi
