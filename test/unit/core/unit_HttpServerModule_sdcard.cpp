/// @module HttpServerModule
/// @also SdCardModule
///
/// `/api/file` and `/api/dir` reach the card through the same path-prefix dispatch every other `fsXxx` call uses.
/// A large transfer resumes a bounded slice per tick (driveSdTransfers) instead of blocking one.
/// Driven over loopback against the real server, as unit_HttpServerModule_connections.cpp does for parking.

#include "doctest.h"
#include "core/system/HttpServerModule.h"
#include "platform/platform.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr uint16_t kTestPort = 18091;
using CR = mm::platform::TcpConnection::ConnectResult;

bool connectAndAccept(mm::HttpServerModule& http, mm::platform::TcpConnection& client) {
    if (!client.connectStart("127.0.0.1", kTestPort)) return false;
    CR r = CR::Pending;
    for (int i = 0; i < 500 && r == CR::Pending; i++) {
        r = client.connectPoll();
        if (r == CR::Pending) mm::platform::delayMs(1);
    }
    if (r != CR::Connected) return false;
    mm::platform::delayMs(5);
    http.tick20ms();
    return true;
}

// Ticks the server, accumulating whatever the client reads, until `out` holds at least `wantAtLeast` bytes or `maxTicks` is spent; returns what was accumulated.
std::string pumpUntil(mm::HttpServerModule& http, mm::platform::TcpConnection& client,
                      size_t wantAtLeast, int maxTicks = 3000) {
    std::string out;
    char buf[4096];
    for (int i = 0; i < maxTicks && out.size() < wantAtLeast; i++) {
        http.tick20ms();
        const int n = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (n > 0) out.append(buf, static_cast<size_t>(n));
        else mm::platform::delayMs(1);
    }
    return out;
}

// A deterministic byte pattern, long enough (> the 4096-byte per-tick chunk) to force several ticks of driveSdTransfers regardless of how fast loopback delivers the bytes.
std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = static_cast<char>('A' + (i % 26));
    return s;
}

// Mounts a real card under a fresh temp dir for one test, and tears it down after.
struct SdFixture {
    char root[256];
    SdFixture() {
        static unsigned counter = 0;
        std::snprintf(root, sizeof(root), "%s/mm_sdhttp_test_%u",
                      std::filesystem::temp_directory_path().string().c_str(), counter++);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        mm::platform::fsSetRoot(root);
        mm::platform::setTestSdCardTotal(1024ULL * 1024 * 1024);   // a realistic-looking default
        mm::platform::SdPinConfig pins{};
        pins.clk = 43; pins.cmd = 44; pins.d0 = 39;
        REQUIRE(mm::platform::sdMount(pins));
    }
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~SdFixture() noexcept {
        mm::platform::sdUnmount();
        mm::platform::fsSetRoot("");
        mm::platform::setTestSdCardTotal(1024ULL * 1024 * 1024);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

}  // namespace

TEST_CASE("a path traversal through /sd is rejected, the same shared guard flash uses") {
    char out[64];
    CHECK_FALSE(mm::HttpServerModule::parseFilePath("path=/sd/../.config/secret", out, sizeof(out)));
    CHECK(mm::HttpServerModule::parseFilePath("path=/sd/ok.bin", out, sizeof(out)));
    CHECK(std::strcmp(out, "/sd/ok.bin") == 0);
}

// The card is a second mount, not a real subdirectory, so the File Manager tree would never offer it without this: a regression guard for exactly the gap found live on the bench, where /sd listed fine once you knew to ask, but the root never advertised it as a child to expand.
TEST_CASE("the root listing advertises /sd as a folder, so the tree can reach it") {
    SdFixture fix;
    struct Seen { std::vector<std::string> names; bool sdIsDir = false; } seen;
    mm::platform::fsList("/", [](const char* name, bool isDir, uint32_t, void* user) {
        auto* s = static_cast<Seen*>(user);
        s->names.push_back(name);
        if (std::string(name) == "sd") s->sdIsDir = isDir;
    }, &seen);
    REQUIRE(std::find(seen.names.begin(), seen.names.end(), "sd") != seen.names.end());
    CHECK(seen.sdIsDir);
    // Not doubled: desktop backs /sd with a real folder under the same root, which the listing must skip.
    CHECK(std::count(seen.names.begin(), seen.names.end(), "sd") == 1);
}

TEST_CASE("an SD upload larger than one tick's chunk streams across several ticks and reads back whole") {
    SdFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client));

    const std::string payload = pattern(10000);
    char req[256];
    const int reqLen = std::snprintf(req, sizeof(req),
        "POST /api/file?path=/sd/big.bin HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", payload.size());
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), static_cast<size_t>(reqLen)));
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));

    const std::string reply = pumpUntil(http, client, 12);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 200", 12) == 0);

    CHECK(mm::platform::fsSize("/sd/big.bin") == static_cast<long>(payload.size()));
    std::vector<char> back(payload.size());
    char chunk[2048];
    size_t got = 0;
    while (got < back.size()) {
        const int n = mm::platform::fsReadAt("/sd/big.bin", static_cast<long>(got), chunk,
                                             std::min(sizeof(chunk), back.size() - got));
        REQUIRE(n > 0);
        std::memcpy(back.data() + got, chunk, static_cast<size_t>(n));
        got += static_cast<size_t>(n);
    }
    CHECK(std::memcmp(back.data(), payload.data(), payload.size()) == 0);

    client.close();
    http.release();
}

TEST_CASE("an SD download larger than one tick's chunk streams across several ticks and arrives whole") {
    SdFixture fix;
    const std::string payload = pattern(10000);
    REQUIRE(mm::platform::fsWriteAtomic("/sd/dl.bin", payload.data(), payload.size()));

    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client));

    const char* req = "GET /api/file?path=/sd/dl.bin HTTP/1.1\r\n\r\n";
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));

    // Header plus the whole body, with slack for the header's own bytes.
    const std::string reply = pumpUntil(http, client, payload.size() + 50);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 200", 12) == 0);
    const auto bodyStart = reply.find("\r\n\r\n");
    REQUIRE(bodyStart != std::string::npos);
    const std::string body = reply.substr(bodyStart + 4);
    CHECK(body.size() == payload.size());
    CHECK(body == payload);

    client.close();
    http.release();
}

TEST_CASE("an SD upload that stalls mid-transfer is closed and its slot frees for the next one") {
    struct Clock { ~Clock() { mm::platform::setTestNowMs(0); } } clock;
    mm::platform::setTestNowMs(1000);
    SdFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client));

    // Declares 10000 bytes, sends only 100, then goes silent: a stuck upload, never completed.
    char req[256];
    const int reqLen = std::snprintf(req, sizeof(req),
        "POST /api/file?path=/sd/stuck.bin HTTP/1.1\r\nContent-Length: 10000\r\n\r\n");
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), static_cast<size_t>(reqLen)));
    const std::string partial = pattern(100);
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(partial.data()), partial.size()));
    http.tick20ms();   // claims the slot, writes the 100 bytes already buffered

    // Comfortably past the stall guard (the real bound is a private implementation detail).
    mm::platform::setTestNowMs(1000 + 60000);
    for (int i = 0; i < 10; i++) http.tick20ms();
    CHECK(client.read(reinterpret_cast<uint8_t*>(req), sizeof(req)) == 0);   // closed, no response

    // The slot is free again: a fresh, small upload still completes normally.
    mm::platform::TcpConnection client2;
    REQUIRE(connectAndAccept(http, client2));
    const char* req2 = "POST /api/file?path=/sd/ok2.bin HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi";
    REQUIRE(client2.write(reinterpret_cast<const uint8_t*>(req2), std::strlen(req2)));
    const std::string reply = pumpUntil(http, client2, 12);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 200", 12) == 0);

    client2.close();
    http.release();
}

TEST_CASE("an SD upload past the card's free space is refused with 507, immediately") {
    SdFixture fix;
    mm::platform::setTestSdCardTotal(500);   // a nearly-full, tiny fictional card
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client));

    const std::string payload = pattern(2000);   // bigger than the fictional card
    char req[256];
    const int reqLen = std::snprintf(req, sizeof(req),
        "POST /api/file?path=/sd/toobig.bin HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", payload.size());
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), static_cast<size_t>(reqLen)));
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));

    const std::string reply = pumpUntil(http, client, 12);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 507", 12) == 0);
    CHECK(mm::platform::fsSize("/sd/toobig.bin") < 0);   // nothing written

    client.close();
    http.release();
}

// A declared length past FAT32's own ceiling (4 GiB-1) is well-formed, just too big: 413, not the generic 400 a malformed header gets; no body is sent, since the ceiling rejects on the header alone.
TEST_CASE("an SD upload declaring more than FAT32's 4 GiB-1 ceiling is refused with 413") {
    SdFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client));

    const char* req = "POST /api/file?path=/sd/huge.bin HTTP/1.1\r\nContent-Length: 5000000000\r\n\r\n";
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));

    const std::string reply = pumpUntil(http, client, 12);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 413", 12) == 0);

    client.close();
    http.release();
}

// Point 1 of the review moved removeRecursiveSd's job onto FilesystemModule::removeTree, which already dispatches through platform::fsList/fsRemove.
// This pins that DELETE /api/dir on /sd actually reaches it, recursing through a non-empty folder exactly as it does for flash.
TEST_CASE("DELETE /api/dir removes a file and a non-empty folder on the card, via removeTree") {
    SdFixture fix;
    REQUIRE(mm::platform::fsWriteAtomic("/sd/lone.bin", "x", 1));
    REQUIRE(mm::platform::fsMkdir("/sd/tree/sub"));
    REQUIRE(mm::platform::fsWriteAtomic("/sd/tree/top.txt", "1", 1));
    REQUIRE(mm::platform::fsWriteAtomic("/sd/tree/sub/leaf.txt", "2", 1));

    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();

    {
        mm::platform::TcpConnection client;
        REQUIRE(connectAndAccept(http, client));
        const char* req = "DELETE /api/dir?path=/sd/lone.bin HTTP/1.1\r\n\r\n";
        REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
        const std::string reply = pumpUntil(http, client, 12);
        CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 200", 12) == 0);
        client.close();
    }
    CHECK(mm::platform::fsSize("/sd/lone.bin") < 0);

    {
        mm::platform::TcpConnection client;
        REQUIRE(connectAndAccept(http, client));
        const char* req = "DELETE /api/dir?path=/sd/tree HTTP/1.1\r\n\r\n";
        REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
        const std::string reply = pumpUntil(http, client, 12);
        CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 200", 12) == 0);
        client.close();
    }
    CHECK(mm::platform::fsSize("/sd/tree/sub/leaf.txt") < 0);
    CHECK_FALSE(mm::platform::fsExists("/sd/tree"));

    http.release();
}
