#pragma once

// Loopback helpers the HTTP server tests share: connect a client and let the server accept it, tick until a reply arrives, read a close, make a payload.

#include "core/system/HttpServerModule.h"
#include "platform/platform.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace http_loopback {

using CR = mm::platform::TcpConnection::ConnectResult;

// Connects `client` to the server on `port`, then ticks once so the server accepts it.
inline bool connectAndAccept(mm::HttpServerModule& http, mm::platform::TcpConnection& client, uint16_t port) {
    if (!client.connectStart("127.0.0.1", port)) return false;
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

// Ticks the server, accumulating what the client reads, until it holds at least `wantAtLeast` bytes or `maxTicks` is spent.
inline std::string pumpUntil(mm::HttpServerModule& http, mm::platform::TcpConnection& client,
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

// What a read answers after the server closes the socket: -1 while the close is in flight, then 0.
inline int readAfterClose(mm::platform::TcpConnection& client) {
    char buf[32];
    int got = -1;
    for (int ms = 0; ms < 200 && got == -1; ms++) {
        got = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (got == -1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return got;
}

// Connects `client`, posts `body` to `path` and returns the reply once its status line has arrived.
inline std::string postSmall(mm::HttpServerModule& http, mm::platform::TcpConnection& client, uint16_t port,
                             const char* path, const std::string& body) {
    if (!connectAndAccept(http, client, port)) return {};
    const std::string req = std::string("POST /api/file?path=") + path + " HTTP/1.1\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    if (!client.write(reinterpret_cast<const uint8_t*>(req.data()), req.size())) return {};
    return pumpUntil(http, client, 12);
}

// A deterministic byte pattern, longer than one 4 KB transfer chunk when `n` is, so an upload spans several ticks.
inline std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = static_cast<char>('A' + (i % 26));
    return s;
}

}  // namespace http_loopback
