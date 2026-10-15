#pragma once

/// @defgroup MqttPacket MQTT 3.1.1 wire framing
/// @{
/// The framing and the handful of control packets a client light needs, in pure C++.
///
/// @attribution MQTT
///
/// @moreinfo
///
/// There are no ESP-IDF or network dependencies here, deliberately, so the packet maths is host-unit-tested against golden byte vectors exactly as the Improv framing is.
/// The live socket lifecycle, its connect, keepalive and reconnect, lives in `MqttModule`; this is only the bytes.
///
/// ## The wire format
///
/// Every packet is a fixed header, a remaining-length varint, then a variable header and payload:
///
/// ```text
/// [type<<4 | flags][remaining-length varint][variable header + payload]
/// ```
///
/// This header owns what a client needs: CONNECT, PUBLISH in both directions, SUBSCRIBE, PINGREQ and DISCONNECT to send, and CONNACK, SUBACK, PINGRESP and PUBLISH to receive.
///
/// ## The remaining-length varint
///
/// One to four bytes, seven data bits each, the high bit meaning more follows.
/// It is the one genuinely fiddly part of MQTT, so it is its own pair of pure functions, boundary-tested at 0, 127, 128, 16383 and 16384.
///
/// ## Two CONNECT fields the spec constrains
///
/// An empty-string username counts as no username, because a UI text control left blank is empty rather than null.
/// The password flag must never be set without the username flag, so a password with no username is dropped.
/// A compliant broker would otherwise reject the whole CONNECT.
///
/// A Will needs both a topic and a payload, since the Will flag gates both length-prefixed fields and a topic with no message is malformed.
/// A Will makes the broker publish it when this client drops ungracefully, on a power cut or a lost network.
/// That is the availability seam Home Assistant greys an entity out on.
/// Retaining it means a late-joining subscriber still sees the device as offline.
///
/// The protocol is the OASIS MQTT 3.1.1 standard, the framing mosquitto, HiveMQ and homebridge-mqttthing speak, written against our own buffers with no library.

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mm {

// --8<-- [start:mqtt-constants]
/// Control packet types, the high nibble of the fixed header's first byte.
enum class MqttPacketType : uint8_t {
    Connect     = 0x1,
    Connack     = 0x2,
    Publish     = 0x3,
    Suback      = 0x9,
    Subscribe   = 0x8,
    Pingreq     = 0xC,
    Pingresp    = 0xD,
    Disconnect  = 0xE,
};

/// The protocol name and level a CONNECT carries in its variable header.
inline constexpr char    kMqttProtocolName[4] = {'M','Q','T','T'};
inline constexpr uint8_t kMqttProtocolLevel   = 0x04;   // 4 == MQTT 3.1.1

/// CONNECT flag bits: clean session, and optionally a username, password and Will.
inline constexpr uint8_t kMqttConnectCleanSession = 0x02;
inline constexpr uint8_t kMqttConnectWillFlag     = 0x04;  // §3.1.2.5 — a Will is present in the payload
inline constexpr uint8_t kMqttConnectWillRetain   = 0x20;  // §3.1.2.7 — the broker publishes the Will retained
inline constexpr uint8_t kMqttConnectPasswordFlag = 0x40;
inline constexpr uint8_t kMqttConnectUsernameFlag = 0x80;
// --8<-- [end:mqtt-constants] (Will QoS sits in bits 3-4, and QoS 0 needs no constant)

/// Encode a remaining length into `out`, returning the bytes written or 0 when it will not fit.
inline size_t encodeRemainingLength(uint32_t value, uint8_t* out) {
    if (value > 268435455u) return 0;   // 0xFFFFFFF — the 4-byte ceiling
    size_t n = 0;
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value > 0) byte |= 0x80;    // more bytes follow
        out[n++] = byte;
    } while (value > 0);
    return n;
}

/// Decode a remaining-length varint, false when the field is truncated or malformed.
inline bool decodeRemainingLength(const uint8_t* in, size_t inLen,
                                  uint32_t* value, size_t* consumed) {
    uint32_t result = 0;
    uint32_t multiplier = 1;
    size_t i = 0;
    for (;;) {
        if (i >= inLen) return false;            // truncated — caller feeds more
        if (i >= 4) return false;                // >4 bytes → malformed varint
        const uint8_t byte = in[i++];
        result += static_cast<uint32_t>(byte & 0x7F) * multiplier;
        if ((byte & 0x80) == 0) break;           // high bit clear → last byte
        multiplier <<= 7;
    }
    *value = result;
    *consumed = i;
    return true;
}

/// Append a length-prefixed string, returning the new write position or 0 on overflow.
inline size_t mqttAppendString(uint8_t* out, size_t outLen, size_t pos,
                               const char* s, size_t len) {
    if (pos + 2 + len > outLen) return 0;
    out[pos++] = static_cast<uint8_t>((len >> 8) & 0xFF);
    out[pos++] = static_cast<uint8_t>(len & 0xFF);
    if (len > 0) { std::memcpy(out + pos, s, len); pos += len; }
    return pos;
}

/// Write just the fixed header, the caller passing the body length it has already computed.
inline size_t mqttWriteFixedHeader(uint8_t* out, size_t outLen,
                                   MqttPacketType type, uint8_t flags, uint32_t bodyLen) {
    // Into a local scratch first, so the size is validated before anything reaches `out`.
    uint8_t rlBuf[4];
    const size_t rl = encodeRemainingLength(bodyLen, rlBuf);
    if (rl == 0 || 1 + rl > outLen) return 0;
    out[0] = static_cast<uint8_t>((static_cast<uint8_t>(type) << 4) | (flags & 0x0F));
    std::memcpy(out + 1, rlBuf, rl);
    return 1 + rl;   // total fixed-header size
}

/// Build a CONNECT, its client id required and everything else optional.
inline size_t buildMqttConnect(const char* clientId,
                               const char* username, const char* password,
                               uint16_t keepaliveSec, uint8_t* out, size_t outLen,
                               const char* willTopic = nullptr, const char* willPayload = nullptr,
                               bool willRetain = false) {
    if (!clientId) return 0;
    // An empty username means none, and a password without one is dropped: see the appendix.
    if (username && !username[0]) username = nullptr;
    if (!username) password = nullptr;
    // A Will needs both a topic and a payload, so a partial one is dropped.
    if (willTopic && !willTopic[0]) willTopic = nullptr;
    if (!willTopic || !willPayload) { willTopic = nullptr; willPayload = nullptr; }
    const size_t idLen   = std::strlen(clientId);
    const size_t userLen = username ? std::strlen(username) : 0;
    const size_t passLen = password ? std::strlen(password) : 0;
    const size_t wtLen   = willTopic ? std::strlen(willTopic) : 0;
    const size_t wpLen   = willPayload ? std::strlen(willPayload) : 0;

    // A ten-byte variable header, then the payload in the spec's order, each field length-prefixed.
    uint8_t connectFlags = kMqttConnectCleanSession;
    if (willTopic) { connectFlags |= kMqttConnectWillFlag; if (willRetain) connectFlags |= kMqttConnectWillRetain; }
    if (username) connectFlags |= kMqttConnectUsernameFlag;
    if (password) connectFlags |= kMqttConnectPasswordFlag;

    const uint32_t bodyLen = static_cast<uint32_t>(
        2 + 4 + 1 + 1 + 2 +                       // proto name(len+"MQTT") + level + flags + keepalive
        2 + idLen +
        (willTopic ? 2 + wtLen + 2 + wpLen : 0) +
        (username ? 2 + userLen : 0) +
        (password ? 2 + passLen : 0));

    size_t pos = mqttWriteFixedHeader(out, outLen, MqttPacketType::Connect, 0, bodyLen);
    if (pos == 0) return 0;

    // Variable header
    pos = mqttAppendString(out, outLen, pos, kMqttProtocolName, 4);   // "MQTT"
    if (pos == 0) return 0;
    if (pos + 4 > outLen) return 0;
    out[pos++] = kMqttProtocolLevel;
    out[pos++] = connectFlags;
    out[pos++] = static_cast<uint8_t>((keepaliveSec >> 8) & 0xFF);
    out[pos++] = static_cast<uint8_t>(keepaliveSec & 0xFF);

    // Payload: the client id, then the Will, username and password, in the spec's order.
    pos = mqttAppendString(out, outLen, pos, clientId, idLen);
    if (pos == 0) return 0;
    if (willTopic) {
        pos = mqttAppendString(out, outLen, pos, willTopic, wtLen);   if (pos == 0) return 0;
        pos = mqttAppendString(out, outLen, pos, willPayload, wpLen); if (pos == 0) return 0;
    }
    if (username) { pos = mqttAppendString(out, outLen, pos, username, userLen); if (pos == 0) return 0; }
    if (password) { pos = mqttAppendString(out, outLen, pos, password, passLen); if (pos == 0) return 0; }
    return pos;
}

/// Build a PUBLISH at QoS 0, `retain` making a late-subscribing hub receive the topic again.
inline size_t buildMqttPublish(const char* topic, const uint8_t* payload, size_t payloadLen,
                               uint8_t* out, size_t outLen, bool retain = false) {
    if (!topic) return 0;
    const size_t topicLen = std::strlen(topic);
    const uint32_t bodyLen = static_cast<uint32_t>(2 + topicLen + payloadLen);   // topic (QoS0: no id)
    size_t pos = mqttWriteFixedHeader(out, outLen, MqttPacketType::Publish,
                                      retain ? 0x1 : 0x0, bodyLen);
    if (pos == 0) return 0;
    pos = mqttAppendString(out, outLen, pos, topic, topicLen);
    if (pos == 0) return 0;
    if (payloadLen > 0) {
        if (pos + payloadLen > outLen) return 0;
        std::memcpy(out + pos, payload, payloadLen);
        pos += payloadLen;
    }
    return pos;
}

/// Build a SUBSCRIBE for one topic filter at QoS 0, its packet id having to be non-zero.
inline size_t buildMqttSubscribe(uint16_t packetId, const char* topic, uint8_t* out, size_t outLen) {
    if (!topic || packetId == 0) return 0;
    const size_t topicLen = std::strlen(topic);
    const uint32_t bodyLen = static_cast<uint32_t>(2 + 2 + topicLen + 1);   // packetId + filter + QoS
    size_t pos = mqttWriteFixedHeader(out, outLen, MqttPacketType::Subscribe, 0x2, bodyLen);
    if (pos == 0) return 0;
    if (pos + 2 > outLen) return 0;
    out[pos++] = static_cast<uint8_t>((packetId >> 8) & 0xFF);
    out[pos++] = static_cast<uint8_t>(packetId & 0xFF);
    pos = mqttAppendString(out, outLen, pos, topic, topicLen);
    if (pos == 0) return 0;
    if (pos + 1 > outLen) return 0;
    out[pos++] = 0x00;   // requested QoS 0
    return pos;
}

/// Build a PINGREQ, which is two bytes.
inline size_t buildMqttPingreq(uint8_t* out, size_t outLen) {
    return mqttWriteFixedHeader(out, outLen, MqttPacketType::Pingreq, 0, 0);
}
/// Build a DISCONNECT, likewise two bytes.
inline size_t buildMqttDisconnect(uint8_t* out, size_t outLen) {
    return mqttWriteFixedHeader(out, outLen, MqttPacketType::Disconnect, 0, 0);
}

/// The largest packet reassembled, control packets a light exchanges being tiny.
inline constexpr size_t kMqttMaxPacket = 512;

/// What feeding a byte to the parser left it holding.
enum class MqttFeedResult : uint8_t {
    NeedMore,       // mid-packet
    PacketReady,    // a complete packet is available via lastType() + (for PUBLISH) topic()/payload()
    Malformed,      // bad varint / oversize packet — parser resynced
};

class MqttInboundParser {
public:
    /// Feed one received byte, the result naming what the parser now holds.
    MqttFeedResult feed(uint8_t byte) {
        switch (state_) {
            case State::FixedHeader:
                type_ = static_cast<uint8_t>(byte >> 4);
                flags_ = static_cast<uint8_t>(byte & 0x0F);
                rlValue_ = 0;
                rlMultiplier_ = 1;
                rlBytes_ = 0;
                state_ = State::RemainingLength;
                return MqttFeedResult::NeedMore;
            case State::RemainingLength: {
                rlValue_ += static_cast<uint32_t>(byte & 0x7F) * rlMultiplier_;
                rlMultiplier_ <<= 7;
                if (++rlBytes_ > 4) { reset(); return MqttFeedResult::Malformed; }  // >4 → malformed
                if ((byte & 0x80) == 0) {                                           // last length byte
                    if (rlValue_ > kMqttMaxPacket) { reset(); return MqttFeedResult::Malformed; }
                    bodyLen_ = rlValue_;
                    bodyPos_ = 0;
                    if (bodyLen_ == 0) { state_ = State::FixedHeader; return MqttFeedResult::PacketReady; }
                    state_ = State::Body;
                }
                return MqttFeedResult::NeedMore;
            }
            case State::Body:
                body_[bodyPos_++] = byte;
                if (bodyPos_ >= bodyLen_) {
                    state_ = State::FixedHeader;
                    return MqttFeedResult::PacketReady;
                }
                return MqttFeedResult::NeedMore;
        }
        return MqttFeedResult::NeedMore;   // unreachable
    }

    /// The completed packet's type.
    uint8_t lastType() const { return type_; }
    /// Its flags nibble.
    uint8_t lastFlags() const { return flags_; }
    /// Its body, whose meaning depends on the type: a CONNACK's return code is the second byte.
    const uint8_t* body() const { return body_; }
    /// How many bytes of the body are filled.
    size_t bodyLen() const { return bodyLen_; }

    /// Split a completed PUBLISH into its topic and payload, false when it is not well-formed.
    bool publish(const char** topic, const uint8_t** payload, size_t* payloadLen) {
        if (type_ != static_cast<uint8_t>(MqttPacketType::Publish)) return false;
        if (bodyLen_ < 2) return false;
        const size_t tLen = static_cast<size_t>((body_[0] << 8) | body_[1]);
        if (2 + tLen > bodyLen_) return false;
        if (tLen >= sizeof(topicBuf_)) return false;
        std::memcpy(topicBuf_, body_ + 2, tLen);
        topicBuf_[tLen] = '\0';
        if (topic) *topic = topicBuf_;
        if (payload) *payload = body_ + 2 + tLen;
        if (payloadLen) *payloadLen = bodyLen_ - 2 - tLen;
        return true;
    }

private:
    void reset() { state_ = State::FixedHeader; }
    enum class State : uint8_t { FixedHeader, RemainingLength, Body };
    State    state_ = State::FixedHeader;
    uint8_t  type_ = 0;
    uint8_t  flags_ = 0;
    uint32_t rlValue_ = 0;
    uint32_t rlMultiplier_ = 1;
    uint8_t  rlBytes_ = 0;
    size_t   bodyLen_ = 0;
    size_t   bodyPos_ = 0;
    uint8_t  body_[kMqttMaxPacket] = {};
    char     topicBuf_[128] = {};
};

/// @}
} // namespace mm
