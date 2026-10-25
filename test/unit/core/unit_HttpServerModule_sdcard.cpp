/// @module HttpServerModule
/// @also SdCardService
///
/// `/api/file` and `/api/dir` reach the card through the `fsXxx` path-prefix dispatch, a large transfer moving a bounded slice per tick, driven over loopback against the real server.

#include "doctest.h"
#include "core/system/HttpServerModule.h"
#include "platform/platform.h"
#include "http_loopback.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t kTestPort = 18091;
using namespace http_loopback;


// Sends a POST of `payload` to `path`: the header, then the whole body.
void postFile(mm::platform::TcpConnection& client, const char* path, const std::string& payload) {
    char req[256];
    const int reqLen = std::snprintf(req, sizeof(req),
        "POST /api/file?path=%s HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", path, payload.size());
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), static_cast<size_t>(reqLen)));
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
}


// Whether a reply holds a whole response: its headers and the Content-Length bytes of body after them.
bool wholeResponse(const std::string& reply) {
    const size_t end = reply.find("\r\n\r\n");
    if (end == std::string::npos) return false;
    const size_t at = reply.find("Content-Length: ");
    const size_t length = at < end ? std::strtoul(reply.c_str() + at + 16, nullptr, 10) : 0;
    return reply.size() >= end + 4 + length;
}

// Sends a whole request and returns the whole response, since macOS loopback can deliver the body a moment after the status line.
std::string request(mm::HttpServerModule& http, mm::platform::TcpConnection& client, const char* req) {
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
    std::string reply = pumpUntil(http, client, 12);
    for (int i = 0; i < 3000 && !wholeResponse(reply); i++) reply += pumpUntil(http, client, 1, 1);
    return reply;
}

// The status line of one request on a connection of its own.
std::string statusOf(mm::HttpServerModule& http, const char* req) {
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));
    const std::string reply = request(http, client, req);
    client.close();
    return reply.substr(0, 12);
}


// Ticks with a millisecond between them, since Linux loopback delivers a write at once and macOS a moment later; a tick with nothing new is harmless.
void tickWhileDelivering(mm::HttpServerModule& http, int ticks = 5) {
    for (int i = 0; i < ticks; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        http.tick20ms();
    }
}


// Connects, declares a 10000-byte upload to `path` but sends only 100, then ticks so the slot claims it: in flight, never finished.
void claimStuckUpload(mm::HttpServerModule& http, mm::platform::TcpConnection& client, const char* path) {
    REQUIRE(connectAndAccept(http, client, kTestPort));
    char req[256];
    const int reqLen = std::snprintf(req, sizeof(req),
        "POST /api/file?path=%s HTTP/1.1\r\nContent-Length: 10000\r\n\r\n", path);
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), static_cast<size_t>(reqLen)));
    const std::string partial = pattern(100);
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(partial.data()), partial.size()));
    tickWhileDelivering(http);
}

// Connects and GETs `path`, returning the response once at least `wantAtLeast` bytes have arrived.
std::string getAndWait(mm::HttpServerModule& http, mm::platform::TcpConnection& client, const char* path,
                       size_t wantAtLeast = 12) {
    REQUIRE(connectAndAccept(http, client, kTestPort));
    char req[160];
    const int n = std::snprintf(req, sizeof(req), "GET /api/file?path=%s HTTP/1.1\r\n\r\n", path);
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), static_cast<size_t>(n)));
    return pumpUntil(http, client, wantAtLeast);
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

// The card is a second mount, not a subdirectory: on the bench /sd listed when asked for, but the root never advertised it to the File Manager tree.
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

TEST_CASE("the root listing does not advertise /sd once the card is unmounted") {
    SdFixture fix;
    REQUIRE(mm::platform::sdUnmount());   // same slot, pins still wired, nothing answering now

    std::vector<std::string> names;
    mm::platform::fsList("/", [](const char* name, bool, uint32_t, void* user) {
        static_cast<std::vector<std::string>*>(user)->push_back(name);
    }, &names);
    CHECK(std::find(names.begin(), names.end(), "sd") == names.end());
}

TEST_CASE("an upload to /sd without a mounted card answers \"no card mounted\", not a generic failure") {
    SdFixture fix;
    REQUIRE(mm::platform::sdUnmount());

    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    const char* req = "POST /api/file?path=/sd/x.bin HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi";
    const std::string reply = request(http, client, req);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 503", 12) == 0);
    CHECK(reply.find("no card mounted") != std::string::npos);

    client.close();
    http.release();
}

TEST_CASE("an SD upload larger than one tick's chunk streams across several ticks and reads back whole") {
    SdFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    const std::string payload = pattern(10000);
    postFile(client, "/sd/big.bin", payload);

    const std::string reply = pumpUntil(http, client, 12);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 200", 12) == 0);

    CHECK(mm::platform::fsSize("/sd/big.bin") == static_cast<int64_t>(payload.size()));
    std::vector<char> back(payload.size());
    mm::platform::FsReadStream rs{};
    REQUIRE(mm::platform::fsReadStreamOpen(rs, "/sd/big.bin"));
    char chunk[2048];
    size_t got = 0;
    while (got < back.size()) {
        const int n = mm::platform::fsReadStreamRead(rs, chunk, std::min(sizeof(chunk), back.size() - got));
        REQUIRE(n > 0);
        std::memcpy(back.data() + got, chunk, static_cast<size_t>(n));
        got += static_cast<size_t>(n);
    }
    mm::platform::fsReadStreamClose(rs);
    CHECK(std::memcmp(back.data(), payload.data(), payload.size()) == 0);

    client.close();
    http.release();
}

// Point 13 of the review: one chunk per tick capped a transfer around 200 KB/s. A payload spanning several 4 KB chunks now commits within a couple of ticks, not one chunk per tick.
TEST_CASE("an SD upload spanning several chunks drains in far fewer ticks than chunks") {
    struct Clock { ~Clock() { mm::platform::setTestNowMs(0); } } clock;
    mm::platform::setTestNowMs(1000);   // frozen, so the per-tick budget cannot run out on a slow machine
    SdFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    const std::string payload = pattern(20000);   // 5 chunks' worth, at the old one-chunk-per-tick rate
    postFile(client, "/sd/multi.bin", payload);

    // The old design needed 5 ticks for the body alone, so committing within 4 proves the drain.
    tickWhileDelivering(http, 4);
    CHECK(mm::platform::fsSize("/sd/multi.bin") == static_cast<int64_t>(payload.size()));

    const std::string reply = pumpUntil(http, client, 12);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 200", 12) == 0);

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
    REQUIRE(connectAndAccept(http, client, kTestPort));

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

    // driveSdDownload closes the read stream the tick after the last byte lands, so a few more ticks pass before the fixture unmounts underneath it.
    for (int i = 0; i < 5; i++) http.tick20ms();
    client.close();
    http.release();
}

// streamFsFile once sent the 200 header before claiming a slot, so a busy slot answered with a body a dead connection never delivered.
TEST_CASE("an SD download with no free send slot answers 503, not a broken 200") {
    struct Heap { ~Heap() { mm::platform::setTestFreeInternalHeap(0); } } heap;   // restored however the test ends
    SdFixture fix;
    REQUIRE(mm::platform::fsWriteAtomic("/sd/noslot.bin", "x", 1));
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();

    // Short of internal RAM, with the turn held elsewhere, every download waits in its slot.
    mm::platform::setTestFreeInternalHeap(mm::HttpServerModule::kSendHeadroom - 1);
    int elsewhere = 0;
    REQUIRE(http.maySendForTest(&elsewhere));
    std::vector<mm::platform::TcpConnection> held(mm::HttpServerModule::kSendSlots);
    for (auto& c : held) {
        REQUIRE(connectAndAccept(http, c, kTestPort));
        const char* req = "GET /api/file?path=/sd/noslot.bin HTTP/1.1\r\n\r\n";
        REQUIRE(c.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
        tickWhileDelivering(http);
    }

    mm::platform::TcpConnection client;
    const std::string reply = getAndWait(http, client, "/sd/noslot.bin");
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 503", 12) == 0);

    client.close();
    for (auto& c : held) c.close();
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
    claimStuckUpload(http, client, "/sd/stuck.bin");   // goes silent after this: a stuck upload, never completed

    // Comfortably past the stall guard (the real bound is a private implementation detail).
    mm::platform::setTestNowMs(1000 + 60000);
    for (int i = 0; i < 10; i++) http.tick20ms();
    CHECK(readAfterClose(client) == 0);   // closed, no response

    mm::platform::TcpConnection client2;
    CHECK(postSmall(http, client2, kTestPort, "/sd/ok2.bin", "hi").rfind("HTTP/1.1 200", 0) == 0);   // the slot is free again

    client2.close();
    http.release();
}

// Point 7 of the review: release() left an active slot's stream and socket open, leaking a FIL (or its desktop equivalent) and a connection.
TEST_CASE("release mid-transfer closes the slot's stream and socket, leaking neither") {
    SdFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    claimStuckUpload(http, client, "/sd/leak.bin");
    REQUIRE(mm::platform::sdCardBusy());   // the open stream holds the busy guard

    http.release();
    CHECK_FALSE(mm::platform::sdCardBusy());   // the stream closed, not abandoned to a dangling handle
    CHECK(readAfterClose(client) == 0);   // the socket closed too
    CHECK_FALSE(std::filesystem::exists(std::string(fix.root) + "/sd/leak.bin.tmp"));   // no leftover temp

    client.close();
}

TEST_CASE("an SD upload past the card's free space is refused with 507, immediately") {
    SdFixture fix;
    mm::platform::setTestSdCardTotal(500);   // a nearly-full, tiny fictional card
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    const std::string payload = pattern(2000);   // bigger than the fictional card
    postFile(client, "/sd/toobig.bin", payload);

    const std::string reply = pumpUntil(http, client, 12);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 507", 12) == 0);
    CHECK(mm::platform::fsSize("/sd/toobig.bin") < 0);   // nothing written

    client.close();
    http.release();
}

// A declared length past FAT32's 4 GiB-1 ceiling is well-formed but too big: 413, not a malformed header's 400, decided on the header alone.
TEST_CASE("an SD upload declaring more than FAT32's 4 GiB-1 ceiling is refused with 413") {
    SdFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    const char* req = "POST /api/file?path=/sd/huge.bin HTTP/1.1\r\nContent-Length: 5000000000\r\n\r\n";
    const std::string reply = request(http, client, req);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 413", 12) == 0);

    client.close();
    http.release();
}

// Review point 1 moved removeRecursiveSd onto FilesystemModule::removeTree; DELETE /api/dir on /sd reaches it, recursing through a non-empty folder as it does on flash.
TEST_CASE("DELETE /api/dir removes a file and a non-empty folder on the card, via removeTree") {
    SdFixture fix;
    REQUIRE(mm::platform::fsWriteAtomic("/sd/lone.bin", "x", 1));
    REQUIRE(mm::platform::fsMkdir("/sd/tree/sub"));
    REQUIRE(mm::platform::fsWriteAtomic("/sd/tree/top.txt", "1", 1));
    REQUIRE(mm::platform::fsWriteAtomic("/sd/tree/sub/leaf.txt", "2", 1));

    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();

    CHECK(statusOf(http, "DELETE /api/dir?path=/sd/lone.bin HTTP/1.1\r\n\r\n") == "HTTP/1.1 200");
    CHECK(mm::platform::fsSize("/sd/lone.bin") < 0);

    CHECK(statusOf(http, "DELETE /api/dir?path=/sd/tree HTTP/1.1\r\n\r\n") == "HTTP/1.1 200");
    CHECK(mm::platform::fsSize("/sd/tree/sub/leaf.txt") < 0);
    CHECK_FALSE(mm::platform::fsExists("/sd/tree"));

    http.release();
}

// Point 5 of the review: DELETE /api/dir?path=/sd must not wipe the card - removeTree refuses the mount root itself, before listing or deleting anything inside it.
TEST_CASE("DELETE /api/dir on /sd itself is refused, leaving the card untouched") {
    SdFixture fix;
    REQUIRE(mm::platform::fsWriteAtomic("/sd/keep.bin", "x", 1));

    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();

    CHECK(statusOf(http, "DELETE /api/dir?path=/sd HTTP/1.1\r\n\r\n") == "HTTP/1.1 500");

    CHECK(mm::platform::fsSize("/sd/keep.bin") >= 0);   // untouched, not even one level in
    http.release();
}

// The filesystems skip repeated slashes, so "/sd//" names the card's root as surely as "/sd" does, and is refused the same way.
TEST_CASE("DELETE /api/dir on the card root spelled with extra slashes is refused too") {
    SdFixture fix;
    REQUIRE(mm::platform::fsWriteAtomic("/sd/keep.bin", "x", 1));
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    for (const char* req : {"DELETE /api/dir?path=/sd// HTTP/1.1\r\n\r\n", "DELETE /api/dir?path=//sd HTTP/1.1\r\n\r\n"})
        CHECK(statusOf(http, req) != "HTTP/1.1 200");
    CHECK(mm::platform::fsSize("/sd/keep.bin") >= 0);
    http.release();
}

// A relative path reaches the card as "/sd/...", so its upload gets the card's size limit, not flash's 8 MB: past the card's free space is 507, not 413.
TEST_CASE("an upload to a relative sd/ path gets the card's size limit") {
    SdFixture fix;
    mm::platform::setTestSdCardTotal(500);
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));
    const char* req = "POST /api/file?path=sd/rel.bin HTTP/1.1\r\nContent-Length: 9000000\r\n\r\n";
    const std::string reply = request(http, client, req);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 507", 12) == 0);
    client.close();
    http.release();
}

// Deleting what is not there answered 500, the code for the device failing; it is the client's mistake, a 404.
TEST_CASE("DELETE /api/dir on a path that names nothing answers 404") {
    SdFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    for (const char* req : {"DELETE /api/dir?path=/sd/nothing.bin HTTP/1.1\r\n\r\n",
                            "DELETE /api/dir?path=/moonlive/nothing.mle HTTP/1.1\r\n\r\n"})
        CHECK(statusOf(http, req) == "HTTP/1.1 404");
    http.release();
}
