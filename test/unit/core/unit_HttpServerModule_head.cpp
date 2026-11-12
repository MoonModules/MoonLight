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

TEST_CASE("a query parameter matches whole: a value is found by its own name only, and a flag only with its own value") {
    using mm::http::queryValue;
    using mm::http::queryHas;
    CHECK(std::string(queryValue("path=/a.json&hidden=1", "path")) == "/a.json&hidden=1");   // up to the caller's '&'
    CHECK(std::string(queryValue("path=/a.json&hidden=1", "hidden")) == "1");
    CHECK(queryValue("xpath=/a.json", "path") == nullptr);
    CHECK(queryValue("pathx=/a.json", "path") == nullptr);
    CHECK(queryValue("", "path") == nullptr);
    CHECK(queryValue(nullptr, "path") == nullptr);
    CHECK(queryHas("live=1", "live", "1"));
    CHECK(queryHas("a=2&live=1&b=3", "live", "1"));
    CHECK_FALSE(queryHas("live=10", "live", "1"));
    CHECK_FALSE(queryHas("xlive=1", "live", "1"));
    CHECK_FALSE(queryHas("live=", "live", "1"));
}

TEST_CASE("a device fetches only an http or https URL, never a file, a relative path or another scheme") {
    using mm::http::isHttpUrl;
    CHECK(isHttpUrl("http://192.168.1.10/fw.bin"));
    CHECK(isHttpUrl("https://github.com/MoonModules/MoonLight/releases/download/v7.0.0/firmware-esp32.bin"));
    CHECK_FALSE(isHttpUrl("file:///etc/passwd"));
    CHECK_FALSE(isHttpUrl("//evil.example/fw.bin"));
    CHECK_FALSE(isHttpUrl("ftp://host/fw.bin"));
    CHECK_FALSE(isHttpUrl(""));
}

TEST_CASE("a page's Origin is trusted when it names this network, an address or a local name, and refused when it names the internet") {
    using mm::http::isTrustedOrigin;
    const char* trusted[] = {
        "http://192.168.1.158", " http://192.168.1.158:8080", "http://10.0.0.5", "http://172.16.0.1", "http://172.31.255.254",
        "http://169.254.1.1", "http://127.0.0.1:8080", "http://localhost:8080", "http://mm-acfd.local", "http://MM-ACFD.LOCAL.",
        "http://lights.home.arpa", "http://nas.internal", "http://moonlight", "http://[::1]:8080", "http://[fe80::1]", "http://[fd12::3]",
        "http://ledstrip.lan", "http://ledstrip.home", "http://ledstrip.localdomain",   // the routers' own defaults
    };
    for (const char* o : trusted) CHECK_MESSAGE(isTrustedOrigin(o), o);
    const char* refused[] = {
        "https://evil.example", "http://192.168.1.158.evil.example", "http://8.8.8.8", "http://172.32.0.1", "http://11.0.0.1",
        "http://local.evil.example", "http://lan.evil.example", "http://evil.homes", "null", "", "http://", "http://[2001:db8::1]", "https://moonmodules.org",
        "null\r\nReferer: http://192.168.1.158/",   // a header without a scheme ends at its own line, never borrowing the next one's host
    };
    for (const char* o : refused) CHECK_MESSAGE(!isTrustedOrigin(o), o);
}

TEST_CASE("an address is this network's when private, a VPN's shared space, link-local or loopback") {
    const uint8_t mine[][4] = {{10, 1, 2, 3}, {172, 16, 0, 1}, {172, 31, 9, 9}, {192, 168, 1, 227}, {100, 64, 0, 1}, {100, 127, 9, 9}, {169, 254, 3, 4}, {127, 0, 0, 1}};
    for (const auto& a : mine) CHECK(mm::isPrivateIPv4(a));
    const uint8_t theirs[][4] = {{8, 8, 8, 8}, {172, 15, 0, 1}, {172, 32, 0, 1}, {192, 169, 0, 1}, {1, 1, 1, 1}, {100, 63, 0, 1}, {100, 128, 0, 1}};
    for (const auto& a : theirs) CHECK_FALSE(mm::isPrivateIPv4(a));
}

TEST_CASE("a request comes from beyond the network by a public Origin, or by a Host a rebinding page could own, never through the access point") {
    using mm::http::fromBeyondNetwork;
    const char* own = "MM-ACFD";
    CHECK(fromBeyondNetwork("GET /api/file?path=/.config/WiFi.json HTTP/1.1\r\nHost: evil.example\r\n\r\n", false, own));   // DNS rebinding: no Origin on a same-origin GET
    CHECK(fromBeyondNetwork("POST /api/reboot HTTP/1.1\r\nHost: 192.168.1.158\r\nOrigin: https://evil.example\r\n\r\n", false, own));
    CHECK(fromBeyondNetwork("GET / HTTP/1.1\r\nHost: mm-acfdx.fritz.box\r\n\r\n", false, own));   // a name that only begins like the device's
    CHECK(fromBeyondNetwork("GET / HTTP/1.1\r\nHost: myhome.dyndns.example:8080\r\n\r\n", false, own));
    const char* served[] = {
        "GET / HTTP/1.1\r\nHost: 192.168.1.158\r\n\r\n", "GET / HTTP/1.1\r\nHost: 192.168.1.158:8080\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: 203.0.113.5\r\n\r\n",                 // a public LAN address: an IP literal, which rebinding cannot use
        "GET / HTTP/1.1\r\nHost: [2001:db8::1]:8080\r\n\r\n", "GET / HTTP/1.1\r\nHost: localhost:8080\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: mm-acfd.local\r\n\r\n", "GET / HTTP/1.1\r\nHost: MM-ACFD.fritz.box\r\n\r\n",   // the router's name for it
        "GET / HTTP/1.1\r\nHost: mm-acfd.lan\r\n\r\n", "GET / HTTP/1.1\r\nHost: mm-acfd.tailnet.ts.net\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: mm-acfd\r\n\r\n", "GET / HTTP/1.1\r\nHost: nas\r\n\r\n",   // one label, which public DNS never serves
        "GET / HTTP/1.1\r\nHost: ledstrip.lan\r\n\r\n",   // a router's own default domain, under a name other than the device's
        "GET / HTTP/1.0\r\n\r\n",
        "POST /api/state HTTP/1.1\r\nHost: 192.168.1.158\r\nOrigin: http://192.168.1.20:8080\r\n\r\n",
        "POST /api/state HTTP/1.1\r\nHost: mm-acfd.fritz.box\r\nOrigin: http://mm-acfd.fritz.box\r\n\r\n",   // its own page under the router's name
    };
    for (const char* r : served) CHECK_MESSAGE(!fromBeyondNetwork(r, false, own), r);
    CHECK(fromBeyondNetwork("GET / HTTP/1.1\r\nHost: mm-acfd.fritz.box\r\n\r\n", false, nullptr));   // a device without a name has no own name to match
    // A phone on the access point checks for a sign-in page under any name, and the UI there is http://4.3.2.1, no private address.
    CHECK_FALSE(fromBeyondNetwork("GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\n\r\n", true, own));
    CHECK_FALSE(fromBeyondNetwork("POST /api/state HTTP/1.1\r\nHost: 4.3.2.1\r\nOrigin: http://4.3.2.1\r\n\r\n", true, own));
}

TEST_CASE("the map on one device drives another when both are named under their router's domain, and a page from another domain is refused") {
    using mm::http::fromBeyondNetwork;
    CHECK_FALSE(fromBeyondNetwork("POST /api/state HTTP/1.1\r\nHost: mm-bbbb.fritz.box\r\nOrigin: http://mm-aaaa.fritz.box\r\n\r\n", false, "MM-BBBB"));
    CHECK_FALSE(fromBeyondNetwork("GET /api/state HTTP/1.1\r\nHost: mm-bbbb.lan:80\r\nOrigin: http://mm-aaaa.lan\r\n\r\n", false, "MM-BBBB"));
    CHECK(fromBeyondNetwork("POST /api/state HTTP/1.1\r\nHost: mm-bbbb.fritz.box\r\nOrigin: http://mm-aaaa.evil.example\r\n\r\n", false, "MM-BBBB"));   // another domain
    CHECK(fromBeyondNetwork("POST /api/state HTTP/1.1\r\nHost: 192.168.1.20\r\nOrigin: http://mm-aaaa.fritz.box\r\n\r\n", false, "MM-BBBB"));   // named by address, no domain to share
    CHECK(fromBeyondNetwork("POST /api/state HTTP/1.1\r\nHost: mm-bbbb.box\r\nOrigin: http://evil.fritz.box\r\n\r\n", false, "MM-BBBB"));   // the domains differ: fritz.box and box
}

TEST_CASE("a page's own name is trusted only on a request to that same name, so a public page named like the device is refused") {
    using mm::http::fromBeyondNetwork;
    const char* own = "MM-ACFD";
    CHECK(fromBeyondNetwork("POST /api/firmware/url HTTP/1.1\r\nHost: 192.168.1.158\r\nOrigin: http://mm-acfd.attacker.example\r\n\r\n", false, own));   // a site named after the device, calling it by address
    CHECK_FALSE(fromBeyondNetwork("POST /api/state HTTP/1.1\r\nHost: mm-acfd.fritz.box\r\nOrigin: http://mm-acfd.fritz.box\r\n\r\n", false, own));   // its own page under the router's name
    CHECK_FALSE(fromBeyondNetwork("POST /api/state HTTP/1.1\r\nHost: 203.0.113.5\r\nOrigin: http://203.0.113.5\r\n\r\n", false, own));   // its own page at a public LAN address
    CHECK(fromBeyondNetwork("POST /api/state HTTP/1.1\r\nHost: 203.0.113.5\r\nOrigin: http://198.51.100.7\r\n\r\n", false, own));   // another public address
}
