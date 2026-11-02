#pragma once

/// The clean stop a service manager asks for, apart from main() so a test can raise it.

#include <csignal>

namespace mm::platform {

/// Clear `flag` on SIGTERM, how systemd, Docker and kill stop a service, so the main loop stops as on Ctrl+C. Not one-shot: a second one must not cut the stop short, and a service manager escalates to SIGKILL itself. Windows never raises SIGTERM.
inline void stopOnSigterm([[maybe_unused]] volatile std::sig_atomic_t& flag) {
#ifndef _WIN32
    static volatile std::sig_atomic_t* target = nullptr;   // a handler takes no argument, so it reaches the flag through this
    target = &flag;
    struct sigaction sa{};
    sa.sa_handler = [](int) { *target = 0; };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

}  // namespace mm::platform
