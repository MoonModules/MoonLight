#pragma once

/// The shared application main every target's entry point hands control to: the ESP32's app_main, the Teensy's setup, the desktop's main and the Android app's service.

#include <atomic>
#include <cstdint>

/// Build the module tree, serve the UI on `httpPort`, and run the render loop while `keepRunning` holds; a board never clears it, a desktop's signal handler or the Android service does.
void mm_main(const std::atomic<bool>& keepRunning, uint16_t httpPort);
