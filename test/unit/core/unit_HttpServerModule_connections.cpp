/// @module HttpServerModule
///
/// A connection whose request arrives late, as one whose first packet was lost and resent does, is parked and served rather than closed with no reply.
/// An embedded UI file streams across ticks from a send slot, so a slow client never holds the render loop for the whole file.
/// Driven over loopback against the real server on a test port, since a running desktop app holds 8080.

#include "doctest.h"
#include "core/system/HttpServerModule.h"
#include "http_loopback.h"
#include "platform/platform.h"
#include "ui/ui_embedded.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace http_loopback;

constexpr uint16_t kTestPort = 18090;

// Tick the server until the client reads a reply or sees the connection closed; the bytes read, 0 when closed.
int readReply(mm::HttpServerModule& http, mm::platform::TcpConnection& client, char* out, size_t len) {
    for (int i = 0; i < 300; i++) {
        http.tick20ms();
        const int n = client.read(reinterpret_cast<uint8_t*>(out), len - 1);
        if (n >= 0) { out[n > 0 ? n : 0] = 0; return n; }
        mm::platform::delayMs(1);
    }
    return -1;
}

}  // namespace

TEST_CASE("HttpServer: a request that arrives after the connection was accepted is served, not dropped") {
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    mm::platform::delayMs(50);   // the request arrives a resend later
    const char* req = "OPTIONS / HTTP/1.1\r\nHost: test\r\n\r\n";
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
    char reply[256];
    CHECK(readReply(http, client, reply, sizeof(reply)) > 0);
    CHECK(std::strncmp(reply, "HTTP/1.1 204", 12) == 0);

    client.close();
    http.release();
}

// Four parking slots dropped the rest of a burst, and a client fetching eight HLS segments at once saw a third of them reset.
TEST_CASE("HttpServer: eight connections opened before their requests are each answered") {
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection clients[8];
    for (auto& c : clients) REQUIRE(connectAndAccept(http, c, kTestPort));   // each accepted with nothing to read: parked
    const char* req = "OPTIONS / HTTP/1.1\r\nHost: test\r\n\r\n";
    for (auto& c : clients) REQUIRE(c.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
    for (auto& c : clients) {
        char reply[256];
        CHECK(readReply(http, c, reply, sizeof(reply)) > 0);
        CHECK(std::strncmp(reply, "HTTP/1.1 204", 12) == 0);
        c.close();
    }
    http.release();
}

TEST_CASE("HttpServer: a connection that never sends a request is closed once its parking time is up") {
    struct Clock { ~Clock() { mm::platform::setTestNowMs(0); } } clock;   // reset however the test ends
    mm::platform::setTestNowMs(1000);
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    http.tick20ms();
    char reply[64];
    CHECK(client.read(reinterpret_cast<uint8_t*>(reply), sizeof(reply)) == -1);   // still open, waiting
    mm::platform::setTestNowMs(1000 + 3000);
    CHECK(readReply(http, client, reply, sizeof(reply)) == 0);                    // closed, with no reply

    client.close();
    http.release();
}

namespace {

// The platform's UI folder for one test, the checkout's again however the test ends.
struct UiFolder {
    explicit UiFolder(const char* path) { mm::platform::setUiFolder(path); }
    ~UiFolder() { mm::platform::setUiFolder("src/ui"); }
};

// The embedded app.js, the largest UI file, the one a slow link spends seconds on.
const mm::ui::EmbeddedFile& embeddedAppJs() {
    for (const auto& f : mm::ui::kFiles)
        if (std::strcmp(f.name, "app.js") == 0) return f;
    return mm::ui::kFiles[0];
}

// Request `path`, with `header` as one more request line when given, then tick and read until the server closes the connection; everything read.
std::string getUntilClosed(mm::HttpServerModule& http, mm::platform::TcpConnection& client, const char* path, const std::string& header = "") {
    char req[192];
    std::snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: test\r\n%s%s\r\n", path, header.c_str(), header.empty() ? "" : "\r\n");
    if (!client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req))) return {};
    return readUntilClosed(http, client);
}

}  // namespace

TEST_CASE("HttpServer: a checkout's UI folder serves its script modules and nothing else in it") {
    const auto dir = std::filesystem::temp_directory_path() / "mm_ui_folder_test";
    const std::string dirName = dir.string();   // the platform keeps the pointer, so the string outlives the server
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "panel.js") << "export const x = 1;";
    std::ofstream(dir / "ui_embedded.h") << "secret";
    mm::HttpServerModule http;
    http.port = kTestPort;
    const UiFolder folder(dirName.c_str());
    http.setup();

    const auto statusOf = [&](const char* path) {
        mm::platform::TcpConnection client;
        REQUIRE(connectAndAccept(http, client, kTestPort));
        const std::string reply = getUntilClosed(http, client, path);
        client.close();
        return reply.substr(0, 12);
    };
    CHECK(statusOf("/panel.js") == "HTTP/1.1 200");
    CHECK(statusOf("/ui_embedded.h") == "HTTP/1.1 404");
    http.release();
    std::filesystem::remove_all(dir);
}

TEST_CASE("HttpServer: an embedded UI file streams across ticks, arrives whole, and its connection then closes") {
    mm::HttpServerModule http;
    http.port = kTestPort;
    const UiFolder embeddedOnly(nullptr);   // no files on disk: the embedded copy is served, as on a device
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    const auto& file = embeddedAppJs();
    const std::string reply = getUntilClosed(http, client, "/app.js");
    const auto bodyStart = reply.find("\r\n\r\n");
    REQUIRE(bodyStart != std::string::npos);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 200", 12) == 0);
    REQUIRE(reply.size() - (bodyStart + 4) == file.len);
    CHECK(std::memcmp(reply.data() + bodyStart + 4, file.data, file.len) == 0);

    client.close();
    http.release();
}

TEST_CASE("HttpServer: a page load past the send slots gets every file whole or a 503, never a write that holds the loop") {
    mm::HttpServerModule http;
    http.port = kTestPort;
    const UiFolder embeddedOnly(nullptr);   // no files on disk: the embedded copy is served, as on a device
    http.setup();
    constexpr int kClients = 18;   // past the sixteen slots
    mm::platform::TcpConnection clients[kClients];
    // Each connects and sends its request at once, as a browser's burst does; a tick between them keeps the listen backlog from filling.
    const char* req = "GET /app.js HTTP/1.1\r\nHost: test\r\n\r\n";
    for (auto& c : clients) {
        REQUIRE(c.connectStart("127.0.0.1", kTestPort));
        CR r = CR::Pending;
        for (int i = 0; i < 500 && r == CR::Pending; i++) {
            r = c.connectPoll();
            if (r == CR::Pending) mm::platform::delayMs(1);
        }
        REQUIRE(r == CR::Connected);
        REQUIRE(c.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
        http.tick20ms();
    }

    const auto& file = embeddedAppJs();
    std::string got[kClients];
    bool closed[kClients] = {};
    char buf[4096];
    for (int i = 0; i < 5000; i++) {
        http.tick20ms();
        bool all = true;
        for (int k = 0; k < kClients; k++) {
            if (closed[k]) continue;
            const int n = clients[k].read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
            if (n > 0) got[k].append(buf, static_cast<size_t>(n));
            else if (n == 0) closed[k] = true;
            all = all && closed[k];
        }
        if (all) break;
    }
    int whole = 0;
    for (int k = 0; k < kClients; k++) {
        CAPTURE(k);
        const auto bodyStart = got[k].find("\r\n\r\n");
        REQUIRE(bodyStart != std::string::npos);
        if (got[k].compare(0, 12, "HTTP/1.1 503") == 0) continue;   // every slot was busy when it asked
        CHECK(got[k].size() - (bodyStart + 4) == file.len);
        whole++;
    }
    CHECK(whole >= 16);   // every slot served
    for (auto& c : clients) c.close();
    http.release();
}

// The preview drains first every tick and offers a frame after each one ends, so it took the lease again at once and a page file below the headroom crawled.
TEST_CASE("HttpServer: below the headroom a send that waited goes before the one whose turn just ended") {
    struct Restore { ~Restore() { mm::platform::setTestFreeInternalHeap(0); } } restore;
    mm::HttpServerModule http;
    int previewSend = 0, fileSend = 0;
    mm::platform::setTestFreeInternalHeap(mm::HttpServerModule::kSendHeadroom - 1);
    CHECK(http.maySendForTest(&previewSend));
    CHECK_FALSE(http.maySendForTest(&fileSend));   // waits its turn
    http.endSendForTest(&previewSend);
    CHECK_FALSE(http.maySendForTest(&previewSend));   // its next frame waits for the file
    CHECK(http.maySendForTest(&fileSend));
    http.endSendForTest(&fileSend);
    CHECK(http.maySendForTest(&previewSend));
}

// Below the headroom files, state streams and preview frames take turns: one holds the lease until it ends, and one stalled past the stall guard hands it on. The queue stays one socket's buffer, and none waits forever.
TEST_CASE("HttpServer: below the headroom one send queues at a time, and a stalled one hands its turn on") {
    struct Restore { ~Restore() { mm::platform::setTestFreeInternalHeap(0); mm::platform::setTestNowMs(0); } } restore;
    mm::HttpServerModule http;
    int fileSend = 0, previewSend = 0;
    mm::platform::setTestNowMs(1000);
    CHECK(http.maySendForTest(&fileSend));     // room enough: every send goes
    CHECK(http.maySendForTest(&previewSend));
    mm::platform::setTestFreeInternalHeap(mm::HttpServerModule::kSendHeadroom - 1);
    CHECK(http.maySendForTest(&fileSend));     // short of room: the first to ask holds the turn
    CHECK_FALSE(http.maySendForTest(&previewSend));
    http.endSendForTest(&fileSend);
    CHECK(http.maySendForTest(&previewSend));  // its send ended: the next one goes
    CHECK_FALSE(http.maySendForTest(&fileSend));
    mm::platform::setTestNowMs(1000 + 11000);  // the holder moved nothing past the stall guard
    CHECK(http.maySendForTest(&fileSend));
}

// A page reload downloaded the whole UI again: each file now carries a tag, and a browser holding it gets a 304 with no body (RFC 9110 section 13.1.2).
TEST_CASE("HttpServer: an embedded file carries its tag, and a request naming that tag is answered 304 with no body") {
    mm::HttpServerModule http;
    http.port = kTestPort;
    const UiFolder embeddedOnly(nullptr);   // no files on disk: the embedded copy is served, as on a device
    http.setup();
    mm::platform::TcpConnection first;
    REQUIRE(connectAndAccept(http, first, kTestPort));
    const std::string full = getUntilClosed(http, first, "/app.js");
    REQUIRE(full.rfind("HTTP/1.1 200", 0) == 0);
    const auto at = full.find("ETag: ");
    REQUIRE(at != std::string::npos);
    const std::string tag = full.substr(at + 6, full.find("\r\n", at) - at - 6);
    CHECK(tag == embeddedAppJs().etag);

    mm::platform::TcpConnection again;
    REQUIRE(connectAndAccept(http, again, kTestPort));
    const std::string reply = getUntilClosed(http, again, "/app.js", "If-None-Match: " + tag);
    CHECK(reply.rfind("HTTP/1.1 304", 0) == 0);
    CHECK(reply.find("\r\n\r\n") + 4 == reply.size());   // headers only
    http.release();
}

// A desktop running from a checkout loads the UI's own modules, which the embedded bundle holds inside app.js, from the folder; nothing outside it.
TEST_CASE("HttpServer: a desktop serves a UI module from its folder, and no path out of it") {
    const std::string dir = (std::filesystem::temp_directory_path() / "mm_ui_disk").string();
    std::filesystem::create_directories(dir);
    { std::ofstream(dir + "/extra.js") << "export const x = 1;"; }
    mm::HttpServerModule http;
    http.port = kTestPort;
    const UiFolder folder(dir.c_str());
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));
    const std::string got = getUntilClosed(http, client, "/extra.js");
    CHECK(got.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(got.find("export const x = 1;") != std::string::npos);
    mm::platform::TcpConnection out;
    REQUIRE(connectAndAccept(http, out, kTestPort));
    CHECK(getUntilClosed(http, out, "/..%2Fextra.js").rfind("HTTP/1.1 404", 0) == 0);
    http.release();
    std::filesystem::remove_all(dir);
}

// A classic ESP32 under a page load and a preview queued sends until 2.6 KB of internal RAM was left, and WiFi, with no room to receive, dropped the board off the network.
TEST_CASE("HttpServer: a send queues nothing while internal RAM is at the floor, and finishes once room returns") {
    struct Heap { ~Heap() { mm::platform::setTestFreeInternalHeap(0); } } heap;   // restored however the test ends
    const UiFolder embeddedOnly(nullptr);
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));
    mm::platform::setTestFreeInternalHeap(mm::HttpServerModule::kSendFloor);
    const char* req = "GET /app.js HTTP/1.1\r\nHost: test\r\n\r\n";
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
    std::string got;
    char buf[4096];
    for (int i = 0; i < 50; i++) {
        http.tick20ms();
        const int n = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (n > 0) got.append(buf, static_cast<size_t>(n));
        mm::platform::delayMs(1);
    }
    const size_t headerEnd = got.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    CHECK(got.size() == headerEnd + 4);   // the header went, no byte of the body
    mm::platform::setTestFreeInternalHeap(0);
    for (int i = 0; i < 3000; i++) {
        http.tick20ms();
        const int n = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (n > 0) got.append(buf, static_cast<size_t>(n));
        else if (n == 0) break;
        else mm::platform::delayMs(1);
    }
    CHECK(got.size() - (headerEnd + 4) == embeddedAppJs().len);
    http.release();
}

// A classic ESP32 keeps 40 to 55 KB free, below the headroom, and no page file went out at all: below it files go one at a time, never none.
TEST_CASE("HttpServer: an embedded file still arrives whole while internal RAM is short") {
    struct Heap { ~Heap() { mm::platform::setTestFreeInternalHeap(0); } } heap;   // restored however the test ends
    mm::HttpServerModule http;
    http.port = kTestPort;
    const UiFolder embeddedOnly(nullptr);   // no files on disk: the embedded copy is served, as on a device
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    mm::platform::setTestFreeInternalHeap(mm::HttpServerModule::kSendHeadroom - 1);
    const std::string got = getUntilClosed(http, client, "/app.js");
    const auto headerEnd = got.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    CHECK(got.size() - (headerEnd + 4) == embeddedAppJs().len);

    client.close();
    http.release();
}
