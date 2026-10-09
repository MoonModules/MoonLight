/// @module HttpServerModule
///
/// A connection whose request arrives late, as one whose first packet was lost and resent does, is parked and served rather than closed with no reply.
/// Driven over loopback against the real server on a test port, since a running desktop app holds 8080.

#include "doctest.h"
#include "core/system/HttpServerModule.h"
#include "platform/platform.h"

#include <cstring>

namespace {

constexpr uint16_t kTestPort = 18090;
using CR = mm::platform::TcpConnection::ConnectResult;

// Connect a client and let the server accept it before any request is sent.
bool connectAndAccept(mm::HttpServerModule& http, mm::platform::TcpConnection& client) {
    if (!client.connectStart("127.0.0.1", kTestPort)) return false;
    CR r = CR::Pending;
    for (int i = 0; i < 500 && r == CR::Pending; i++) {
        r = client.connectPoll();
        if (r == CR::Pending) mm::platform::delayMs(1);
    }
    if (r != CR::Connected) return false;
    mm::platform::delayMs(5);
    http.tick20ms();   // accepts the connection: nothing to read yet
    return true;
}

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
    REQUIRE(connectAndAccept(http, client));

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
    REQUIRE(connectAndAccept(http, client));

    http.tick20ms();
    char reply[64];
    CHECK(client.read(reinterpret_cast<uint8_t*>(reply), sizeof(reply)) == -1);   // still open, waiting
    mm::platform::setTestNowMs(1000 + 3000);
    CHECK(readReply(http, client, reply, sizeof(reply)) == 0);                    // closed, with no reply

    client.close();
    http.release();
}
