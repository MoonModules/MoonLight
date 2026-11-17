#pragma once

/// The clean stop a service manager asks for, apart from main() so a test can raise it.

#include <atomic>
#include <csignal>

namespace mm::platform {

/// Clear `flag` on SIGTERM, how systemd, Docker and kill stop a service, so the main loop stops as on Ctrl+C. Not one-shot: a second one must not cut the stop short, and a service manager escalates to SIGKILL itself. Windows never raises SIGTERM.
inline void stopOnSigterm([[maybe_unused]] std::atomic<bool>& flag) {
#ifndef _WIN32
    static std::atomic<bool>* target = nullptr;   // a handler takes no argument, so it reaches the flag through this
    target = &flag;
    struct sigaction sa{};
    sa.sa_handler = [](int) { target->store(false); };   // lock-free, so a handler may store it
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

}  // namespace mm::platform
