#pragma once
/// The RTSP control conversation, RFC 2326: one client's request becomes one response and a state move.
/// @moreinfo
/// ## An alternative is judged whole
/// RFC 2326 lets a client offer several transports in one Transport header, comma separated and in preference order.
/// Each carries its own parameters, so a scan finding the protocol in one and the port in another accepts a combination the client never offered.
/// `RTP/AVP/TCP;interleaved=0-1,RTP/AVP;multicast;client_port=6000` would read as unicast UDP on port 6000, which is neither thing asked for.
/// So each alternative is judged as a unit and the first usable one wins.
/// Usable means RTP/AVP, not the interleaved TCP profile, explicitly unicast, and carrying a port.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm::rtsp {

/// The port RTSP is registered on, which every client tries first.
static constexpr uint16_t kPort = 554;

/// How far a session has progressed, which decides what the next request may ask for.
enum class State : uint8_t {
    Init,      ///< connected, and describing or setting up from here
    Ready,     ///< SETUP agreed a transport, so PLAY may start the stream
    Playing,   ///< packets are flowing to the client's RTP port
};

/// What one request asked for, parsed out of its first line and headers.
struct Request {
    enum class Verb : uint8_t { Unknown, Options, Describe, Setup, Play, Teardown };
    Verb     verb   = Verb::Unknown;   ///< which verb, or Unknown where the line names none
    uint32_t cseq   = 0;        ///< echoed in the response, which is how a client pairs the two
    uint16_t rtpPort = 0;       ///< the client's RTP port, from SETUP's Transport header
    bool     unicastUdp = false; ///< true where SETUP asked for RTP/AVP/UDP unicast
};

/// Compare `n` bytes without regard to case, which is how RFC 2326 reads a header name.
inline bool ieq(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (x != y) return false;
    }
    return true;
}

/// Compare `n` bytes at `at` against a literal, false where the buffer holds fewer.
inline bool matchAt(const char* buf, size_t len, size_t at, const char* lit, size_t n) {
    return at + n <= len && std::memcmp(buf + at, lit, n) == 0;
}

/// A decimal number at `at`, bounded by the buffer and by `limit`; `limit + 1` where it overflows or holds no digit.
inline uint32_t boundedDecimal(const char* buf, size_t len, size_t at, uint32_t limit) {
    uint32_t v = 0;
    size_t digits = 0;
    for (; at < len && buf[at] >= '0' && buf[at] <= '9'; at++, digits++) {
        const uint32_t d = static_cast<uint32_t>(buf[at] - '0');
        // Tested BEFORE the multiply, since `v * 10 + d` wraps past the type's range and lands back under the limit.
        if (v > (limit - d) / 10) return limit + 1;
        v = v * 10 + d;
    }
    return digits ? v : limit + 1;
}

/// Read the Transport header, taking the first alternative this server can carry.
inline void parseTransport(const char* buf, size_t len, Request* out) {
    // The header's own span, since these tokens are common enough to appear elsewhere in a request.
    size_t at = len, end = len;
    for (size_t i = 0; i + 10 <= len; i++) {
        if (ieq(buf + i, "Transport:", 10)) { at = i + 10; break; }
    }
    if (at >= len) return;
    for (size_t i = at; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') { end = i; break; }
    }

    while (at < end) {
        size_t stop = at;
        while (stop < end && buf[stop] != ',') stop++;      // one alternative, comma to comma
        const size_t n = stop - at;
        bool rtpAvp = false, tcpProfile = false, unicast = false, multicast = false;
        uint16_t port = 0;
        for (size_t j = at; j < stop; j++) {
            const size_t left = stop - j;
            if (!rtpAvp && left >= 7 && matchAt(buf, stop, j, "RTP/AVP", 7)) {
                rtpAvp = true;
                tcpProfile = left >= 11 && matchAt(buf, stop, j, "RTP/AVP/TCP", 11);
            }
            if (left >= 7 && matchAt(buf, stop, j, "unicast", 7)) unicast = true;
            if (left >= 9 && matchAt(buf, stop, j, "multicast", 9)) multicast = true;
            if (left >= 12 && matchAt(buf, stop, j, "client_port=", 12)) {
                // 0 stays refused (SETUP reads it as "no port"), and 65535 has no pair for RTCP.
                const uint32_t v = boundedDecimal(buf, stop, j + 12, 65534u);
                if (v >= 1 && v <= 65534u) port = static_cast<uint16_t>(v);
            }
        }
        (void)n;
        if (rtpAvp && !tcpProfile && unicast && !multicast && port != 0) {
            out->unicastUdp = true;
            out->rtpPort = port;
            return;                                          // the first usable alternative wins
        }
        at = stop + 1;
    }
}

/// Parse a request, false where the buffer holds no complete first line. A client orders its headers as it likes, so each is searched for by name rather than counted to.
inline bool parseRequest(const char* buf, size_t len, Request* out) {
    if (!buf || !out || len == 0) return false;
    *out = Request{};

    struct { const char* name; Request::Verb verb; } kVerbs[] = {
        {"OPTIONS", Request::Verb::Options},   {"DESCRIBE", Request::Verb::Describe},
        {"SETUP", Request::Verb::Setup},       {"PLAY", Request::Verb::Play},
        {"TEARDOWN", Request::Verb::Teardown},
    };
    for (const auto& v : kVerbs) {
        const size_t n = std::strlen(v.name);
        if (len > n && std::strncmp(buf, v.name, n) == 0 && buf[n] == ' ') {
            out->verb = v.verb;
            break;
        }
    }
    if (out->verb == Request::Verb::Unknown) return false;

    // CSeq, matched a letter at a time and bounded by `len`: RFC 2326 headers are case-insensitive, and these bytes arrive with no terminator to stop a scan.
    for (size_t i = 0; i + 5 <= len; i++) {
        if (ieq(buf + i, "CSeq:", 5)) {
            size_t at = i + 5;
            while (at < len && (buf[at] == ' ' || buf[at] == '\t')) at++;
            const uint32_t v = boundedDecimal(buf, len, at, 0xFFFFFFFEu);
            if (v <= 0xFFFFFFFEu) out->cseq = v;
            break;
        }
    }
    parseTransport(buf, len, out);
    return true;
}

/// One client's conversation: what it has agreed to, and what it is told next.
class Session {
public:
    /// `sessionId` identifies this conversation in every response after SETUP.
    explicit Session(uint32_t sessionId) : id_(sessionId) {}

    /// Where the conversation has reached.
    State state() const { return state_; }
    /// The client's RTP port, once SETUP has named one.
    uint16_t rtpPort() const { return rtpPort_; }
    /// The session id a client echoes back.
    uint32_t id() const { return id_; }

    /// Answer one request into `out`, returning the bytes written; `sdp` and `url` serve DESCRIBE.
    size_t respond(const Request& req, const char* sdp, const char* url,
                   char* out, size_t outLen) {
        switch (req.verb) {
        case Request::Verb::Options:
            return header(out, outLen, 200, "OK", req.cseq,
                          "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n", nullptr);

        case Request::Verb::Describe: {
            if (!sdp) return simple(out, outLen, 500, "Internal Server Error", req.cseq);
            char extra[160];
            std::snprintf(extra, sizeof(extra),
                          "Content-Type: application/sdp\r\nContent-Base: %s\r\nContent-Length: %u\r\n",
                          url ? url : "", static_cast<unsigned>(std::strlen(sdp)));
            return header(out, outLen, 200, "OK", req.cseq, extra, sdp);
        }

        case Request::Verb::Setup: {
            // UDP is the transport this server speaks; anything else is told so plainly.
            if (!req.unicastUdp || req.rtpPort == 0)
                return simple(out, outLen, 461, "Unsupported Transport", req.cseq);
            rtpPort_ = req.rtpPort;
            state_ = State::Ready;
            char extra[200];
            std::snprintf(extra, sizeof(extra),
                          "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u\r\n"
                          "Session: %u\r\n",
                          static_cast<unsigned>(rtpPort_), static_cast<unsigned>(rtpPort_ + 1),
                          static_cast<unsigned>(kServerRtpPort),
                          static_cast<unsigned>(kServerRtpPort + 1),
                          static_cast<unsigned>(id_));
            return header(out, outLen, 200, "OK", req.cseq, extra, nullptr);
        }

        case Request::Verb::Play: {
            // A transport is agreed at SETUP, so PLAY before it has nowhere to send.
            if (state_ != State::Ready && state_ != State::Playing)
                return simple(out, outLen, 455, "Method Not Valid In This State", req.cseq);
            state_ = State::Playing;
            char extra[120];
            std::snprintf(extra, sizeof(extra), "Session: %u\r\nRange: npt=0.000-\r\n",
                          static_cast<unsigned>(id_));
            return header(out, outLen, 200, "OK", req.cseq, extra, nullptr);
        }

        case Request::Verb::Teardown: {
            state_ = State::Init;
            rtpPort_ = 0;
            char extra[64];
            std::snprintf(extra, sizeof(extra), "Session: %u\r\n", static_cast<unsigned>(id_));
            return header(out, outLen, 200, "OK", req.cseq, extra, nullptr);
        }

        case Request::Verb::Unknown:
        default:
            return simple(out, outLen, 501, "Not Implemented", req.cseq);
        }
    }

    /// The port this server sends RTP from, which SETUP advertises.
    static constexpr uint16_t kServerRtpPort = 5004;

private:
    /// A response line, the always-present headers, optional extra headers and an optional body.
    static size_t header(char* out, size_t outLen, int code, const char* reason, uint32_t cseq,
                         const char* extra, const char* body) {
        if (!out || outLen == 0) return 0;
        const int n = std::snprintf(out, outLen,
                                    "RTSP/1.0 %d %s\r\nCSeq: %u\r\n%s\r\n%s",
                                    code, reason, static_cast<unsigned>(cseq),
                                    extra ? extra : "", body ? body : "");
        return (n > 0 && static_cast<size_t>(n) < outLen) ? static_cast<size_t>(n) : 0;
    }

    static size_t simple(char* out, size_t outLen, int code, const char* reason, uint32_t cseq) {
        return header(out, outLen, code, reason, cseq, nullptr, nullptr);
    }

    uint32_t id_;
    State    state_ = State::Init;
    uint16_t rtpPort_ = 0;
};

/// The SDP a DESCRIBE answers with, one H.264 stream at the session's payload type: its length.
inline size_t buildSdp(char* out, size_t outLen, const char* ip, uint16_t width, uint16_t height,
                       uint8_t fps, uint8_t payloadType) {
    if (!out || outLen == 0) return 0;
    // a=framesize and a=framerate are advisory, and a player that ignores them reads the same geometry out of the stream's own SPS.
    const int n = std::snprintf(out, outLen,
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=MoonLight\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP %u\r\n"
        "a=rtpmap:%u H264/90000\r\n"
        "a=framesize:%u %u-%u\r\n"
        "a=framerate:%u\r\n"
        "a=control:*\r\n",
        ip ? ip : "0.0.0.0",
        static_cast<unsigned>(payloadType), static_cast<unsigned>(payloadType),
        static_cast<unsigned>(payloadType), static_cast<unsigned>(width),
        static_cast<unsigned>(height), static_cast<unsigned>(fps));
    return (n > 0 && static_cast<size_t>(n) < outLen) ? static_cast<size_t>(n) : 0;
}

}  // namespace mm::rtsp
