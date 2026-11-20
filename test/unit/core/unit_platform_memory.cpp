/// @module platform

/// Pins the platform's answer to how much memory a new allocation can count on: MemAvailable read from a line of /proc/meminfo, and no figure, which sets no limit, where the line is another or holds no number.

#include "doctest.h"
#include "platform/platform.h"

#include <cstring>

namespace {

// The bytes MemAvailable names in one /proc/meminfo line.
uint64_t availableIn(const char* line) { return mm::platform::availableMemoryFromMeminfoLine(line, std::strlen(line)); }

}  // namespace

TEST_CASE("Memory: the available figure is the MemAvailable line, read in kB") {
    CHECK(availableIn("MemAvailable:    5242880 kB\n") == 5242880ull * 1024);
}

TEST_CASE("Memory: a line without the MemAvailable key, a key inside another line, a key with no number and an empty line give no figure") {
    CHECK(availableIn("MemTotal: 100 kB\n") == 0);
    CHECK(availableIn("MemFree:          912340 kB\n") == 0);
    CHECK(availableIn("Shadow MemAvailable: 99 kB\n") == 0);   // the key opens a line
    CHECK(availableIn("MemAvailable:\n") == 0);                // no number
    CHECK(availableIn("") == 0);
    CHECK(mm::platform::availableMemoryFromMeminfoLine(nullptr, 0) == 0);
}

TEST_CASE("Memory: a figure past 4 GB stays whole") {
    CHECK(availableIn("MemAvailable: 16777216 kB\n") == 16ull * 1024 * 1024 * 1024);
}

TEST_CASE("Memory: the line parser reads the line with or without its newline, and only the length given") {
    CHECK(availableIn("MemAvailable:    5242880 kB") == 5242880ull * 1024);   // fgets leaves the newline off the last line of a file that lacks one
    CHECK(mm::platform::availableMemoryFromMeminfoLine("MemAvailable: 100 kB\nMemFree: 7 kB", 20) == 100ull * 1024);   // only the length given is read
}

#ifdef __linux__
TEST_CASE("Memory: on Linux the platform reads a figure from /proc/meminfo") {
    CHECK(mm::platform::availableMemory() > 0);
}
#endif
