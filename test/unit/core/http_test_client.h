#pragma once

/// Connecting a test client to a real HttpServerModule over loopback, shared by the server tests that drive it.

#include "core/system/HttpServerModule.h"
#include "platform/platform.h"

/// Connect `client` to the server on `port` and let it accept, before any request is sent.
inline bool connectAndAccept(mm::HttpServerModule& http, mm::platform::TcpConnection& client, uint16_t port) {
    using CR = mm::platform::TcpConnection::ConnectResult;
    if (!client.connectStart("127.0.0.1", port)) return false;
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
