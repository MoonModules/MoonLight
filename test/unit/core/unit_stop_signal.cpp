/// @module platform

/// SIGTERM, the stop systemd, Docker and kill send, clears the flag the main loop watches, as Ctrl+C does.

#include "doctest.h"
#include "platform/desktop/stop_signal.h"

#include <csignal>

TEST_CASE("SIGTERM clears the stop flag, and a second one is caught as well, since a service manager escalates itself") {
#ifndef _WIN32
    struct RestoreHandler {   // the test process keeps its default SIGTERM however the case leaves
        struct sigaction old{};
        RestoreHandler() { sigaction(SIGTERM, nullptr, &old); }
        ~RestoreHandler() { sigaction(SIGTERM, &old, nullptr); }
    } restore;
    volatile std::sig_atomic_t running = 1;
    mm::platform::stopOnSigterm(running);
    std::raise(SIGTERM);
    CHECK(running == 0);
    running = 1;
    std::raise(SIGTERM);   // not one-shot: the default action would end this process here
    CHECK(running == 0);
#endif
}
