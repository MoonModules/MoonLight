#pragma once

/// The select() wait the BSD-socket platforms share, desktop and ESP32; included after the platform's socket headers, which declare select and fd_set.

#include "platform/platform.h"   // delayMs

#include <cstdint>

namespace mm::platform::detail {

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
