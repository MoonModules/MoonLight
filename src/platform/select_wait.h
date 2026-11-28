#pragma once

/// The BSD-socket code the host and the ESP32 share: the select() wait and a socket's bound port.

#include "platform/platform_clock.h"   // delayMs

#include <cstdint>
#if defined(_WIN32)
#include <winsock2.h>     // select and fd_set
#include <ws2tcpip.h>     // socklen_t
#else
#include <sys/select.h>   // select and fd_set, which ESP-IDF routes to lwIP
#include <sys/socket.h>   // getsockname
#include <netinet/in.h>   // sockaddr_in, ntohs
#endif

namespace mm::platform::detail {

/// The port `fd` is bound to, through `toSock`, the platform's socket handle; 0 when unbound.
template <typename ToSock>
uint16_t boundPort(int fd, ToSock toSock) {
    if (fd < 0) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    return ::getsockname(toSock(fd), reinterpret_cast<sockaddr*>(&addr), &len) == 0 ? ntohs(addr.sin_port) : 0;
}

/// Which of `fds` select left set, bit i for fds[i]; -1 is a socket left out of the wait.
template <typename ToSock>
int readableMask(const int* fds, int count, fd_set* readable, ToSock toSock) {
    int mask = 0;
    for (int i = 0; i < count; i++)
        if (fds[i] >= 0 && FD_ISSET(toSock(fds[i]), readable)) mask |= 1 << i;
    return mask;
}

/// select() over `fds` through `toSock`, the platform's socket handle, the textbook wait for a receive task: the readable mask, 0 on a timeout.
template <typename ToSock>
int selectReadable(int* fds, int count, uint32_t timeoutMs, ToSock toSock) {
    fd_set readable;
    FD_ZERO(&readable);
    int maxFd = -1;
    for (int i = 0; i < count; i++) {
#ifndef _WIN32
        if (fds[i] >= FD_SETSIZE) fds[i] = -1;   // FD_SET past the set's size is undefined on POSIX
#endif
        if (fds[i] < 0) continue;
        FD_SET(toSock(fds[i]), &readable);
        if (fds[i] > maxFd) maxFd = fds[i];
    }
    if (maxFd < 0) { delayMs(timeoutMs); return 0; }   // nothing open: wait out the period rather than spin
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeoutMs / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeoutMs % 1000) * 1000);
    const int r = ::select(maxFd + 1, &readable, nullptr, nullptr, &tv);
    if (r < 0) delayMs(timeoutMs);   // a failing select waits out the period rather than spin the receive task
    return r > 0 ? readableMask(fds, count, &readable, toSock) : 0;
}

}  // namespace mm::platform::detail
