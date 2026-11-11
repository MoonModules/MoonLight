#pragma once
/// The client side of ESPHome's native update protocol on TCP 3232: what to send next and what each answer byte means, so a MoonLight device hands an ESPHome device the migration image.
///
/// @technical
///
/// **The exchange, as ESPHome's own client (`espota2.py`) runs it.** Magic bytes; OK and version; our feature byte; its answer; its auth byte; the size, 4 bytes big-endian; OK; the MD5 in hex; OK; the image; "received", "update end"; our OK.
///
/// **Plain only.** No compression, password or encryption: the feature byte offers none, and a device asking for a password fails with that reason.
/// ESPHome's ready-made firmware asks for none.
///
/// **The image passes through.** The caller sends the image's bytes itself, in whatever pieces they arrive, and reports them.
/// Protocol version 2 answers one "chunk OK" per 8 KB received and one for the remainder, which this counts in passing.
///
/// Pure: bytes in, bytes out, no socket, so a desktop test runs a whole session; nothing here allocates.
#include "core/util/HttpHead.h"   // queryValue: the relay's query read as its parameters
#include "core/util/Ipv4.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm::esphomeota {

inline constexpr uint16_t kPort = 3232;                                     ///< ESPHome's update port
inline constexpr uint8_t kMagic[5] = {0x6C, 0x26, 0xF7, 0x5C, 0x45};        ///< what opens the exchange
inline constexpr uint32_t kBlock = 8192;                                    ///< the bytes each "chunk OK" stands for
/// The device's answers: below 0x80 progress, from 0x80 an error.
enum Reply : uint8_t {
    kOk = 0x00, kRequestAuth = 0x01, kRequestSha256Auth = 0x02, kHeaderOk = 0x40, kAuthOk = 0x41, kPrepareOk = 0x42,
    kMd5Ok = 0x43, kReceiveOk = 0x44, kEndOk = 0x45, kSupportsCompression = 0x46, kChunkOk = 0x47, kFeatureFlags = 0x48, kFirstError = 0x80,
};

/// The relay's query, `host=<dotted quad>&md5=<32 lowercase hex digits>`, into `host`, `ip` and `md5`; false when either is missing, malformed or followed by more.
inline bool parseRelayQuery(const char* query, char (&host)[16], uint8_t (&ip)[4], char (&md5)[33]) {
    host[0] = md5[0] = '\0';
    const char* h = http::queryValue(query, "host");
    const char* m = http::queryValue(query, "md5");
    if (!h || !m || std::sscanf(h, "%15[0-9.]", host) != 1 || std::sscanf(m, "%32[0-9a-f]", md5) != 1) return false;
    const auto whole = [](const char* at, size_t n) { return at[n] == '&' || at[n] == '\0'; };
    return std::strlen(md5) == 32 && whole(h, std::strlen(host)) && whole(m, 32) && parseDottedQuad(host, ip);
}

/// One update, driven by the caller's socket.
class Client {
public:
    /// Where the exchange stands.
    enum class State : uint8_t { Greet, Version, Features, Auth, Prepare, Md5, Data, Commit, Closing, Done, Failed };

    /// Start an update of `size` bytes whose MD5 is `md5hex` (32 hex characters): the magic bytes go first.
    void begin(uint32_t size, const char* md5hex) {
        *this = Client{};
        size_ = size;
        std::memcpy(md5_, md5hex, 32);
        queue(kMagic, sizeof(kMagic));
        state_ = State::Greet;
    }

    /// The handshake bytes to send now, `n` of them; none during the image, which the caller sends itself.
    const uint8_t* pending(size_t& n) const { n = outLen_ - outAt_; return out_ + outAt_; }

    /// `n` of the pending bytes went out.
    void sent(size_t n) {
        outAt_ += n;
        if (outAt_ < outLen_) return;
        outAt_ = outLen_ = 0;
        if (state_ == State::Greet) state_ = State::Version;
        else if (state_ == State::Closing) state_ = State::Done;
    }

    /// One byte the device answered.
    void received(uint8_t b) {
        if (state_ == State::Done || state_ == State::Failed) return;
        if (b >= kFirstError) { fail("the device refused the update", b); return; }   // its error code says why: ESPHome's espota2.py names each
        (this->*kOnByte[static_cast<uint8_t>(state_)])(b);
    }

    /// `n` image bytes went out; the last of them moves the exchange on to the device's commit.
    void dataSent(size_t n) {
        sentBytes_ += static_cast<uint32_t>(n);
        if (state_ == State::Data && sentBytes_ >= size_) state_ = State::Commit;
    }

    /// Where the exchange stands now.
    State state() const { return state_; }
    /// Whether the image's bytes are due now.
    bool wantsData() const { return state_ == State::Data && sentBytes_ < size_; }
    /// Why it failed, a constant; "" while it has not.
    const char* error() const { return error_; }
    /// The device's error byte behind a failure, 0 when there was none.
    uint8_t errorCode() const { return errorCode_; }
    /// The protocol version the device speaks, 1 or 2.
    uint8_t protocolVersion() const { return version_; }
    /// The "chunk OK"s counted so far.
    uint32_t acks() const { return acks_; }

private:
    void queue(const uint8_t* data, size_t n) {
        std::memcpy(out_ + outLen_, data, n);
        outLen_ += n;
    }
    void fail(const char* why, uint8_t code = 0) { state_ = State::Failed; error_ = why; errorCode_ = code; }
    void version(uint8_t b) {
        if (versionAt_++ == 0) { if (b != kOk) fail("not ESPHome's update", b); return; }
        if (b != 1 && b != 2) { fail("an update protocol version this client does not speak", b); return; }
        version_ = b;
        const uint8_t none = 0;   // no compression, no password, no encryption, no extended protocol
        queue(&none, 1);
        state_ = State::Features;
    }
    void features(uint8_t b) {
        if (b == kFeatureFlags && !flagsNext_) { flagsNext_ = true; return; }   // a flags byte follows, which this client needs nothing from
        state_ = State::Auth;
    }
    void auth(uint8_t b) {
        if (b == kRequestAuth || b == kRequestSha256Auth) { fail("the device asks for a password"); return; }
        if (b != kAuthOk) { fail("an unexpected answer to the handshake", b); return; }
        const uint8_t sz[4] = {uint8_t(size_ >> 24), uint8_t(size_ >> 16), uint8_t(size_ >> 8), uint8_t(size_)};
        queue(sz, 4);
        state_ = State::Prepare;
    }
    void prepared(uint8_t b) {
        if (b != kPrepareOk) { fail("an unexpected answer to the handshake", b); return; }
        queue(reinterpret_cast<const uint8_t*>(md5_), 32);
        state_ = State::Md5;
    }
    void md5Taken(uint8_t b) {
        if (b != kMd5Ok) { fail("an unexpected answer to the handshake", b); return; }
        state_ = State::Data;
    }
    void chunk(uint8_t b) { if (b == kChunkOk) acks_++; }
    void outOfTurn(uint8_t b) { fail("an answer out of turn", b); }
    void commit(uint8_t b) {
        if (b == kChunkOk) { acks_++; return; }   // the last acks, behind the data
        if (b == kReceiveOk) { received_ = true; return; }
        if (b == kEndOk && received_) { const uint8_t ok = kOk; queue(&ok, 1); state_ = State::Closing; return; }
        fail("an unexpected answer after the image", b);
    }

    /// What a byte means in each state, in State's order: a byte while ours are still going out, or after the end, is out of turn.
    using OnByte = void (Client::*)(uint8_t);
    static constexpr OnByte kOnByte[] = {&Client::outOfTurn, &Client::version, &Client::features, &Client::auth, &Client::prepared,
                                         &Client::md5Taken, &Client::chunk, &Client::commit, &Client::outOfTurn, &Client::outOfTurn, &Client::outOfTurn};
    static_assert(sizeof(kOnByte) / sizeof(kOnByte[0]) == static_cast<size_t>(State::Failed) + 1, "one entry per state");

    State state_ = State::Greet;
    uint32_t size_ = 0, sentBytes_ = 0, acks_ = 0;
    char md5_[32] = {};
    uint8_t out_[40] = {};
    size_t outLen_ = 0, outAt_ = 0;
    uint8_t version_ = 0, versionAt_ = 0, errorCode_ = 0;
    bool flagsNext_ = false, received_ = false;
    const char* error_ = "";
};

}  // namespace mm::esphomeota
