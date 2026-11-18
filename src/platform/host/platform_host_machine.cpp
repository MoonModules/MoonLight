// SPDX-License-Identifier: GPL-3.0-or-later
/// The host's answer to what machine it is (platform/machine.h): the kernel's device tree on Linux, `Other` elsewhere, read once.

#include "platform/machine.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace mm::platform {

namespace {

int testMachine_ = -1;       // the test lever: -1 the host answers, else the Machine it names
int restMachine_ = -1;       // what the lever returns to: -1 the host, else the Machine a test binary rests at

#if defined(__linux__)
// The device tree answers when it can be read, whatever it names. A host that cannot read it, such as a container that masks it, falls to /proc/cpuinfo's Model line.
Machine readHost() {
    char data[512];
    size_t n = 0;
    if (FILE* f = std::fopen("/proc/device-tree/compatible", "rb")) {
        n = std::fread(data, 1, sizeof(data), f);
        std::fclose(f);
    }
    if (n > 0) return machineFromCompatible(data, n);
    // cpuinfo lists a core's lines per CPU and the board's Model last, so the whole file is read, up to a bound.
    static char text[32768];
    size_t len = 0;
    if (FILE* f = std::fopen("/proc/cpuinfo", "rb")) {
        len = std::fread(text, 1, sizeof(text), f);
        std::fclose(f);
    }
    return machineFromCpuinfo(text, len);
}
#else
Machine readHost() { return Machine::Other; }
#endif

}  // namespace

namespace {

// The board's model from one cpuinfo line, "Model<blanks>:<blanks>name": the name, or null for any other line.
const char* modelValue(const char* line, size_t len, size_t& valueLen) {
    if (len < 5 || std::strncmp(line, "Model", 5) != 0) return nullptr;
    size_t i = 5;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] != ':') return nullptr;
    i++;
    while (i < len && line[i] == ' ') i++;
    valueLen = len - i;
    return line + i;
}

// The model's words: "Raspberry Pi 5 Model B Rev 1.0", "Raspberry Pi Compute Module 5 Rev 1.0", "Raspberry Pi 500 Rev 1.0".
Machine machineFromModel(const std::string& model) {
    if (model.rfind("Raspberry Pi", 0) != 0) return Machine::Other;
    const std::string rest = model.substr(12);
    const bool five = rest.rfind(" 5 ", 0) == 0 || rest == " 5" || rest.rfind(" 500", 0) == 0 || rest.rfind(" Compute Module 5", 0) == 0;
    return five ? Machine::Pi5 : Machine::PiGpio;
}

// The device tree's compatible strings that name a Pi 5 (the first match wins) and those that name any other Pi.
constexpr const char* kPi5Compatible[] = {"raspberrypi,5", "brcm,bcm2712"};
constexpr const char* kPiCompatible[] = {"raspberrypi,", "brcm,bcm27", "brcm,bcm28"};

template <size_t N>
bool startsWithAny(const char* s, size_t len, const char* const (&prefixes)[N]) {
    for (const char* prefix : prefixes) {
        const size_t p = std::strlen(prefix);
        if (len >= p && std::strncmp(s, prefix, p) == 0) return true;
    }
    return false;
}

}  // namespace

Machine machineFromCpuinfo(const char* text, size_t length) {
    for (size_t at = 0; text != nullptr && at < length;) {
        const char* line = text + at;
        size_t len = 0;
        while (at + len < length && line[len] != '\n') len++;
        at += len + 1;
        size_t valueLen = 0;
        if (const char* value = modelValue(line, len, valueLen)) return machineFromModel(std::string(value, valueLen));
    }
    return Machine::Other;
}

Machine machineFromCompatible(const char* data, size_t length) {
    bool pi = false;
    for (size_t at = 0; data != nullptr && at < length;) {
        const char* s = data + at;
        const size_t room = length - at;
        size_t len = 0;
        while (len < room && s[len] != '\0') len++;
        if (startsWithAny(s, len, kPi5Compatible)) return Machine::Pi5;
        pi = pi || startsWithAny(s, len, kPiCompatible);
        at += len + 1;
    }
    return pi ? Machine::PiGpio : Machine::Other;
}

Machine hostMachine() {
    static const Machine host = readHost();   // once: the board does not change under a running program
    return host;
}

Machine machine() {
    if (testMachine_ >= 0) return static_cast<Machine>(testMachine_);
    return hostMachine();
}

void setTestMachine(Machine kind) { testMachine_ = static_cast<int>(kind); }

void clearTestMachine() { testMachine_ = restMachine_; }

void setTestMachineRest(Machine kind) {
    restMachine_ = static_cast<int>(kind);
    testMachine_ = restMachine_;
}

}  // namespace mm::platform
