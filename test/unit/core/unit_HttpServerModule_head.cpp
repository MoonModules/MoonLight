/// @module HttpServerModule
/// @also FirmwareUpdateModule

/// Reading a request head, which the app's server and MoonBase share: header lookup, the body's length, the 100 Continue handshake, and reads that hold up however TCP splits the bytes.

#include "doctest.h"
#include "core/util/HttpHead.h"
#include "core/util/FirmwareImage.h"

#include <cstring>
#include <string>
#include <vector>

namespace {
// A peer that delivers `bytes` in the given piece sizes, then closes; it records what the server sent back.
struct FakePeer {
    std::string bytes;
    std::vector<size_t> pieces;
    size_t at = 0, piece = 0;
    int operator()(char* buf, size_t cap) {
        if (at >= bytes.size()) return 0;
        size_t n = piece < pieces.size() ? pieces[piece++] : bytes.size() - at;
        if (n > cap) n = cap;
        if (n > bytes.size() - at) n = bytes.size() - at;
        std::memcpy(buf, bytes.data() + at, n);
        at += n;
        return static_cast<int>(n);
    }
};

// A reference to a peer, as the reads take their recv by value or by forwarding.
struct PeerRef {
    FakePeer* p;
    int operator()(char* buf, size_t cap) const { return (*p)(buf, cap); }
};
}  // namespace

TEST_CASE("HTTP header names match case-insensitively, so any client's Content-Length counts") {
    // RFC 9112 makes field names case-insensitive; a case-sensitive finder reads a lowercase `content-length:` as no length and commits an empty file with a 200.
    const char* req = "POST /api/file?path=/x HTTP/1.1\r\ncontent-length: 831\r\n\r\nbody";
    const char* hit = mm::http::findHeader(req, "Content-Length:");
    REQUIRE(hit != nullptr);
    CHECK(std::strncmp(hit, "content-length:", 15) == 0);
    CHECK(mm::http::findHeader("Content-Length: 5\r\n", "content-length:") != nullptr);
    CHECK(mm::http::findHeader("CONTENT-LENGTH: 5\r\n", "Content-Length:") != nullptr);
    CHECK(mm::http::findHeader("GET / HTTP/1.1\r\nHost: x\r\n", "Content-Length:") == nullptr);
    // Anchored at line starts: an X-prefixed lookalike is not the header.
    CHECK(mm::http::findHeader("POST / HTTP/1.1\r\nX-Content-Length: 9\r\n\r\n", "Content-Length:") == nullptr);
    // Bounded by the blank line: a header name inside the body is data, not a header.
    CHECK(mm::http::findHeader("POST / HTTP/1.1\r\nHost: x\r\n\r\nContent-Length: 4", "Content-Length:") == nullptr);
}

TEST_CASE("Content-Length is one whole decimal number, or the request is refused") {
    uint64_t n = 0;
    CHECK(mm::http::contentLength("POST / HTTP/1.1\r\nContent-Length: 2458064\r\n\r\n", n) == mm::http::Length::Valid);
    CHECK(n == 2458064);
    CHECK(mm::http::contentLength("POST / HTTP/1.1\r\nContent-Length:\t0\r\n\r\n", n) == mm::http::Length::Valid);
    CHECK(n == 0);
    // An /sd upload's 4 GiB-1 ceiling needs 64 bits, wider than `unsigned long` on a 32-bit chip.
    CHECK(mm::http::contentLength("POST / HTTP/1.1\r\nContent-Length: 4294967295\r\n\r\n", n) == mm::http::Length::Valid);
    CHECK(n == 4294967295ULL);
    CHECK(mm::http::contentLength("GET / HTTP/1.1\r\nHost: x\r\n\r\n", n) == mm::http::Length::Absent);
    for (const char* bad : {"abc", "12abc", "-5", "+5", "", "99999999999999999999999"}) {
        const std::string req = std::string("POST / HTTP/1.1\r\nContent-Length: ") + bad + "\r\n\r\n";
        CHECK_MESSAGE(mm::http::contentLength(req.c_str(), n) == mm::http::Length::Invalid, bad);
    }
}

TEST_CASE("two Content-Length lines that disagree refuse the request, two that agree frame it") {
    uint64_t n = 0;
    // Two values leave the body's framing to whoever reads which line: the request-smuggling ambiguity RFC 9112 § 6.3 refuses.
    CHECK(mm::http::contentLength("POST / HTTP/1.1\r\nContent-Length: 4\r\nHost: x\r\ncontent-length: 40\r\n\r\n", n) ==
          mm::http::Length::Invalid);
    CHECK(mm::http::contentLength("POST / HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\n", n) == mm::http::Length::Valid);
    CHECK(n == 4);
    CHECK(mm::http::contentLength("POST / HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: x\r\n\r\n", n) == mm::http::Length::Invalid);
}

TEST_CASE("a client holding its body for 100 Continue is recognized, and only that one") {
    // curl sends `Expect: 100-continue` for any body over 1 MB and holds the body until the interim response or a 1 s timeout.
    CHECK(mm::http::expectsContinue("POST /api/firmware/upload HTTP/1.1\r\nExpect: 100-continue\r\n\r\n"));
    CHECK(mm::http::expectsContinue("POST / HTTP/1.1\r\nexpect:100-Continue\r\n\r\n"));
    CHECK_FALSE(mm::http::expectsContinue("POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\n"));
    CHECK_FALSE(mm::http::expectsContinue("POST / HTTP/1.1\r\nExpect: 100-continued\r\n\r\n"));
    CHECK_FALSE(mm::http::expectsContinue("POST / HTTP/1.1\r\nX-Expect: 100-continue\r\n\r\n"));
    CHECK_FALSE(mm::http::expectsContinue("POST / HTTP/1.1\r\n\r\nExpect: 100-continue"));
    CHECK(std::strcmp(mm::http::kContinue, "HTTP/1.1 100 Continue\r\n\r\n") == 0);
}

TEST_CASE("the head is read whole however the bytes are split, and the body's first bytes are kept") {
    const std::string head = "POST /api/firmware/upload HTTP/1.1\r\nHost: 4.3.2.1\r\nContent-Length: 6\r\n\r\n";
    for (const std::vector<size_t>& pieces : {std::vector<size_t>{}, {1, 1, 1}, {10, 30, 5}, {head.size() - 2, 2}, {head.size() + 3}}) {
        FakePeer peer{head + "ABCDEF", pieces};
        char buf[1024];
        size_t headLen = 0;
        const size_t got = mm::http::readHead(buf, sizeof(buf), PeerRef{&peer}, headLen);
        CHECK(headLen == head.size());
        CHECK(got >= headLen);
        CHECK(std::strncmp(buf + headLen, "ABCDEF", got - headLen) == 0);
    }
}

TEST_CASE("a head that never ends stops at the buffer, and a peer that leaves stops the read") {
    FakePeer flood{std::string(4000, 'x'), {}};
    char buf[64];
    size_t headLen = 0;
    CHECK(mm::http::readHead(buf, sizeof(buf), PeerRef{&flood}, headLen) == sizeof(buf) - 1);
    CHECK(headLen == sizeof(buf) - 1);
    CHECK(buf[sizeof(buf) - 1] == '\0');

    FakePeer gone{"GET / HT", {}};
    CHECK(mm::http::readHead(buf, sizeof(buf), PeerRef{&gone}, headLen) == 8);
    CHECK(headLen == 8);   // no blank line: everything held counts as head, and no body
}

TEST_CASE("an upload whose body comes after the head still gets its image identified") {
    // The case curl's 100 Continue produces: the head read ends at the blank line with no body byte, so the identifying bytes come in later reads, split anywhere.
    std::string image(mm::firmware::kIdentifyBytes + 50, '\0');
    for (size_t i = 0; i < image.size(); i++) image[i] = static_cast<char>(i * 7);
    for (const std::vector<size_t>& pieces : {std::vector<size_t>{}, {1}, {mm::firmware::kIdentifyBytes - 1, 1}, {3, 3, 3}}) {
        FakePeer peer{image, pieces};
        char buf[4096];
        const size_t have = mm::http::fillTo(buf, 0, mm::firmware::kIdentifyBytes, PeerRef{&peer});
        CHECK(have == mm::firmware::kIdentifyBytes);
        CHECK(std::memcmp(buf, image.data(), have) == 0);
    }
    // Bytes already in hand count, and the read never runs past what it was asked for.
    FakePeer rest{image.substr(10), {}};
    char buf[4096];
    std::memcpy(buf, image.data(), 10);
    CHECK(mm::http::fillTo(buf, 10, mm::firmware::kIdentifyBytes, PeerRef{&rest}) == mm::firmware::kIdentifyBytes);
    CHECK(rest.at == mm::firmware::kIdentifyBytes - 10);
    CHECK(std::memcmp(buf, image.data(), mm::firmware::kIdentifyBytes) == 0);
}

TEST_CASE("an image shorter than its identifying bytes comes up short, so the install refuses it") {
    FakePeer peer{std::string(20, 'x'), {5, 5}};
    char buf[4096];
    CHECK(mm::http::fillTo(buf, 0, mm::firmware::kIdentifyBytes, PeerRef{&peer}) == 20);
}
