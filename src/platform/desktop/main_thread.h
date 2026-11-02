#pragma once

/// The main thread's loop on macOS, where AppKit serves windows on that thread only: main_desktop.cpp runs MoonLight on a thread of its own and hands the main thread here.

#include <atomic>

namespace mm::platform {

/// Serve the main queue, and AppKit's events once a window made the process an application, until `appDone` is set: after MoonLight's loop returned, so its teardown can still reach the main thread.
void runMainThread(const std::atomic<bool>& appDone);

}  // namespace mm::platform
