#pragma once

/// The wire framing for Improv-WiFi provisioning, in pure C++ with no ESP-IDF or network headers.
///
/// @technical
///
/// ## The wire format
///
/// Every frame is the same shape, per the [Improv serial spec](https://www.improv-wifi.com/serial/):
///
/// ```text
/// [I][M][P][R][O][V][version=1][type][length][payload...length][checksum]
/// ```
///
/// The parser is a state machine fed one byte at a time, and the builder writes the same shape into a caller-owned buffer.
///
/// ## Why framing is split from the RPC semantics
///
/// The payload inside a frame is an Improv RPC body, which the upstream `improv/improv` library parses through `improv::parse_improv_data`.
/// Keeping that out of this header buys three things:
///
/// - The framing is unit-tested on the host, in `test/test_improv_frame.cpp`.
/// - The ESP32 task in `platform_esp32.cpp` stays thin, feeding UART bytes in and reacting to whole frames, including both headers and dispatching at the boundary.
/// - The builder serves both the ESP32 send path and `moondeck/build/improv_provision.py`, which reimplements the same wire format in Python for the provisioning CLI.

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mm {

// --8<-- [start:frame-constants]
/// Framing constants, matching the spec verbatim so the header needs no library include.
inline constexpr uint8_t kImprovMagic[6] = {'I','M','P','R','O','V'};
inline constexpr uint8_t kImprovSerialVersion = 1;
inline constexpr size_t  kImprovMaxPayload    = 128;  // RPC bodies are well under this

/// Frame types from the spec, named without the prefix so they never shadow the library's own `improv::ImprovSerialType` where both are in scope.
enum class ImprovFrameType : uint8_t {
    CurrentState = 0x01,
    ErrorState   = 0x02,
    Rpc          = 0x03,
    RpcResponse  = 0x04,
};
// --8<-- [end:frame-constants]

/// Result of feeding a byte to the parser.
enum class ImprovFeedResult : uint8_t {
    NeedMore,        // mid-frame; keep feeding
    FrameReady,      // a complete, checksum-valid frame is in lastType()/lastPayload()
    BadChecksum,     // a complete frame arrived but the checksum is wrong; dropped
    OversizePayload, // length byte > kImprovMaxPayload; resync
};

/// Byte-at-a-time framing parser, one per UART channel, resetting to the magic search after every frame it completes or drops.
class ImprovFrameParser {
public:
    /// Feed one received byte, the result naming what the parser now holds.
    ImprovFeedResult feed(uint8_t byte) {
        switch (state_) {
            case State::Magic0: case State::Magic1: case State::Magic2:
            case State::Magic3: case State::Magic4: case State::Magic5: {
                const size_t i = static_cast<size_t>(state_);
                if (byte == kImprovMagic[i]) {
                    headerBytes_[i] = byte;
                    state_ = static_cast<State>(i + 1);
                } else {
                    // Resync: if this byte starts the magic, take it.
                    state_ = State::Magic0;
                    if (byte == kImprovMagic[0]) {
                        headerBytes_[0] = byte;
                        state_ = State::Magic1;
                    }
                }
                return ImprovFeedResult::NeedMore;
            }
            case State::Version:
                if (byte == kImprovSerialVersion) {
                    headerBytes_[6] = byte;
                    state_ = State::Type;
                } else {
                    // A bad version resyncs at Magic1 when the byte is itself an 'I', so a frame behind a corrupted header is not lost.
                    state_ = State::Magic0;
                    if (byte == kImprovMagic[0]) {
                        headerBytes_[0] = byte;
                        state_ = State::Magic1;
                    }
                }
                return ImprovFeedResult::NeedMore;
            case State::Type:
                type_ = byte;
                headerBytes_[7] = byte;
                state_ = State::Length;
                return ImprovFeedResult::NeedMore;
            case State::Length:
                expectedLen_ = byte;
                headerBytes_[8] = byte;
                payloadPos_ = 0;
                if (byte > kImprovMaxPayload) {
                    state_ = State::Magic0;
                    return ImprovFeedResult::OversizePayload;
                }
                state_ = (byte > 0) ? State::Payload : State::Checksum;
                return ImprovFeedResult::NeedMore;
            case State::Payload:
                payload_[payloadPos_++] = byte;
                if (payloadPos_ >= expectedLen_) state_ = State::Checksum;
                return ImprovFeedResult::NeedMore;
            case State::Checksum: {
                uint32_t sum = 0;
                for (int i = 0; i < 9; i++) sum += headerBytes_[i];
                for (uint8_t i = 0; i < expectedLen_; i++) sum += payload_[i];
                const bool ok = (static_cast<uint8_t>(sum & 0xFF) == byte);
                state_ = State::Magic0;  // ready for next frame regardless
                return ok ? ImprovFeedResult::FrameReady : ImprovFeedResult::BadChecksum;
            }
        }
        return ImprovFeedResult::NeedMore;  // unreachable; quiets some compilers
    }

    /// The type byte of the last completed frame.
    uint8_t        lastType()       const { return type_; }
    /// The last completed frame's payload, valid until the next @ref feed.
    const uint8_t* lastPayload()    const { return payload_; }
    /// How many bytes of @ref lastPayload the last frame filled.
    uint8_t        lastPayloadLen() const { return expectedLen_; }

private:
    enum class State : uint8_t {
        Magic0, Magic1, Magic2, Magic3, Magic4, Magic5,
        Version, Type, Length, Payload, Checksum
    };
    State   state_       = State::Magic0;
    uint8_t headerBytes_[9] = {};   // magic(6) + version + type + length
    uint8_t type_        = 0;
    uint8_t expectedLen_ = 0;
    uint8_t payload_[kImprovMaxPayload] = {};
    uint8_t payloadPos_  = 0;
};

/// The spec's checksum, a sum taken modulo 256, exposed so builders and tests share one copy.
inline uint8_t improvChecksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return static_cast<uint8_t>(sum & 0xFF);
}

/// Build one frame into `out`, returning the bytes written, or 0 when it does not fit.
inline size_t buildImprovFrame(ImprovFrameType type,
                               const uint8_t* payload, size_t payloadLen,
                               uint8_t* out, size_t outLen) {
    if (payloadLen > kImprovMaxPayload) return 0;
    if (out == nullptr) return 0;                       // no buffer to write to
    if (payloadLen > 0 && payload == nullptr) return 0; // can't memcpy from null
    const size_t need = 6 + 1 + 1 + 1 + payloadLen + 1;
    if (outLen < need) return 0;
    size_t p = 0;
    std::memcpy(out + p, kImprovMagic, 6); p += 6;
    out[p++] = kImprovSerialVersion;
    out[p++] = static_cast<uint8_t>(type);
    out[p++] = static_cast<uint8_t>(payloadLen);
    if (payloadLen > 0) {
        std::memcpy(out + p, payload, payloadLen);
        p += payloadLen;
    }
    out[p] = improvChecksum(out, p);
    return p + 1;
}

} // namespace mm
