/// @module platform

/// Pins platform::TcpConnection::connectStart/connectPoll, the NON-BLOCKING outbound TCP client primitive MqttModule uses to reach a broker without stalling Scheduler::tick.
///
/// connectStart resolves a host (name or dotted-quad via getaddrinfo) and kicks off a non-blocking connect; connectPoll reports Pending/Connected/Failed without blocking.
///
/// Driven over loopback against a real TcpServer (deterministic on host): a listening port connects + the server accepts; a dead port / bad host fails cleanly (no hang, no crash).

#include "doctest.h"
#include "platform/platform.h"

#include <cstdint>

using namespace mm;

// connectStart/connectPoll, the NON-BLOCKING outbound connect MqttModule uses so it never stalls Scheduler::tick (reviewer #2). connectStart returns immediately; connectPoll reports Pending/Connected/Failed without blocking. Over loopback the connect completes within a few polls.
TEST_CASE("platform::TcpConnection::connectStart/connectPoll is non-blocking") {
    using CR = platform::TcpConnection::ConnectResult;
    platform::TcpServer server;
    uint16_t port = 0;
    for (uint16_t p = 34700; p < 34740; p++) { if (server.open(p)) { port = p; break; } }
    REQUIRE(port != 0);

    platform::TcpConnection client;
    REQUIRE(client.connectStart("127.0.0.1", port));   // returns immediately, connect in flight
    // Poll until connected, accepting on the server side and yielding a little between polls so the loopback TCP handshake can complete (connectPoll never blocks, so a tight spin would just burn 200 microseconds before the kernel finishes the handshake).
    platform::TcpConnection accepted;
    CR r = CR::Pending;
    for (int i = 0; i < 200 && r == CR::Pending; i++) {
        if (!accepted.valid()) accepted = server.accept();
        r = client.connectPoll();
        if (r == CR::Pending) platform::delayMs(1);
    }
    CHECK(r == CR::Connected);
    CHECK(client.valid());

    for (int i = 0; i < 100 && !accepted.valid(); i++) { accepted = server.accept(); platform::delayMs(1); }
    CHECK(accepted.valid());

    client.close();
    server.close();
}

// connectStart on a bad/empty host fails immediately (no hang); connectPoll on an unstarted connection reports Failed rather than blocking.
TEST_CASE("platform::TcpConnection::connectStart rejects a bad host; poll on no-fd is Failed") {
    platform::TcpConnection client;
    CHECK_FALSE(client.connectStart("", 1883));
    CHECK_FALSE(client.connectStart(nullptr, 1883));
    CHECK(client.connectPoll() == platform::TcpConnection::ConnectResult::Failed);
}
