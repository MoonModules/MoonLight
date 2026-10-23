/// @module HttpServerModule
///
/// A connection whose request arrives late, as one whose first packet was lost and resent does, is parked and served rather than closed with no reply.
/// An embedded UI file streams across ticks from a send slot, so a slow client never holds the render loop for the whole file.
/// Driven over loopback against the real server on a test port, since a running desktop app holds 8080.

#include "doctest.h"
#include "core/system/HttpServerModule.h"
#include "http_test_client.h"
#include "platform/platform.h"
#include "ui/ui_embedded.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

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

// The embedded app.js, the largest UI file, the one a slow link spends seconds on.
const mm::ui::EmbeddedFile& embeddedAppJs() {
    for (const auto& f : mm::ui::kFiles)
        if (std::strcmp(f.name, "app.js") == 0) return f;
    return mm::ui::kFiles[0];
}

// Request `path`, then tick and read until the server closes the connection; everything read.
std::string getUntilClosed(mm::HttpServerModule& http, mm::platform::TcpConnection& client, const char* path) {
    char req[96];
    std::snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", path);
    if (!client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req))) return {};
    std::string out;
    char buf[4096];
    for (int i = 0; i < 3000; i++) {
        http.tick20ms();
        const int n = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (n > 0) out.append(buf, static_cast<size_t>(n));
        else if (n == 0) break;   // the server closed: the body is complete
        else mm::platform::delayMs(1);
    }
    return out;
}

}  // namespace

TEST_CASE("HttpServer: an embedded UI file streams across ticks, arrives whole, and its connection then closes") {
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setUiPath("/nonexistent-ui");   // no files on disk: the embedded copy is served, as on a device
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
    http.setUiPath("/nonexistent-ui");
    http.setup();
    constexpr int kClients = 18;   // past the sixteen slots
    mm::platform::TcpConnection clients[kClients];
    // Each connects and sends its request at once, as a browser's burst does; a tick between them keeps the listen backlog from filling.
    using CR = mm::platform::TcpConnection::ConnectResult;
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

// A page load over a slow link queued 52 KB of unacknowledged segments on a board without PSRAM, taking it to 3.6 KB free: below the headroom the file waits rather than queues more.
TEST_CASE("HttpServer: an embedded file waits while internal RAM is short, then arrives whole") {
    struct Heap { ~Heap() { mm::platform::setTestFreeInternalHeap(0); } } heap;   // restored however the test ends
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setUiPath("/nonexistent-ui");
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    mm::platform::setTestFreeInternalHeap(mm::HttpServerModule::kSendHeadroom - 1);
    const char* req = "GET /app.js HTTP/1.1\r\nHost: test\r\n\r\n";
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
    std::string held;
    char buf[4096];
    for (int i = 0; i < 50; i++) {
        http.tick20ms();
        const int n = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (n > 0) held.append(buf, static_cast<size_t>(n));
        else mm::platform::delayMs(1);
    }
    const auto headerEnd = held.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    CHECK(held.size() == headerEnd + 4);   // the header went out, no body while short

    mm::platform::setTestFreeInternalHeap(0);
    std::string rest = held;
    for (int i = 0; i < 3000; i++) {
        http.tick20ms();
        const int n = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (n > 0) rest.append(buf, static_cast<size_t>(n));
        else if (n == 0) break;
        else mm::platform::delayMs(1);
    }
    CHECK(rest.size() - (headerEnd + 4) == embeddedAppJs().len);

    client.close();
    http.release();
}
