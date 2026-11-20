// SPDX-License-Identifier: GPL-3.0-or-later

/// The `PioBackend` factory: the stub when asked for it, else the piolib backend, or null when the build has no piolib; and the messages the piolib backend gives, as text a host test can read (see hub75_pio_backend.h).

#include "platform/rpi/hub75_pio_backend.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace mm::platform {

namespace {

// A program opens the real backend freely. The test binary closes the gate before every case, and a case may reopen it; the real backend then opens only when MM_TEST_REAL_PIO=1 (hub75_pio_backend.h, Real-backend gate).
std::atomic<PioBackendGate> gate_{PioBackendGate::Program};
std::atomic<uint32_t> realOpens_{0};   // real backends created

// Whether the test binary's rule keeps the real backend closed now.
bool refusedByTest() { return pioBackendGateRefuses(gate_.load(), pioBackendRealOptedIn()); }

}  // namespace

// The environment is read at each call, so a case that sets or clears the variable sees its own value.
bool pioBackendRealOptedIn() {
    const char* value = std::getenv("MM_TEST_REAL_PIO");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

PioBackend* createPioBackend(bool preferStub) {
    if (preferStub) return createStubBackend();
    if (refusedByTest()) return nullptr;   // refused here, before piolib or /dev/pio0 is looked at
#ifdef MM_HUB75_PIO_HAVE_PIOLIB
    realOpens_++;
    return createPiolibBackend();
#else
    return nullptr;   // no piolib in this build: the engine says so
#endif
}

const char* pioBackendRefusal() {
    return refusedByTest() ? kPioTestRefusal : "needs a Raspberry Pi build with piolib";
}

bool pioBackendGateOpen() { return gate_.load() != PioBackendGate::Closed; }

void setPioBackendGateOpen(bool open) { gate_.store(open ? PioBackendGate::Reopened : PioBackendGate::Closed); }

uint32_t pioBackendRealOpens() { return realOpens_.load(); }

std::string pioModeString(const PioNode& node) {
    std::string out(1, node.characterDevice ? 'c' : '-');
    const char* const rwx = "rwxrwxrwx";
    for (int bit = 8; bit >= 0; bit--) out += (node.mode & (1u << bit)) ? rwx[8 - bit] : '-';
    return out;
}

std::string pioOpenFailure(int err, const PioNode* node) {
    if (err == EINVAL || err == ENOENT)
        return "no /dev/pio0 on this system (pio_open(0): " + std::string(std::strerror(err)) +
               "): is this a Pi 5 whose kernel has the RP1 PIO driver? piolib looks for /dev/pio* once per process, so a node that appears later needs a restart";
    std::string why = std::string("cannot open /dev/pio0: ") + std::strerror(err);
    if (err == EACCES && node != nullptr) {
        why += " (the node is " + pioModeString(*node) + " " + node->owner + " " + node->group;
        // The group's access decides: a node that gives its group no read and write is for the owner alone, and no group helps.
        if ((node->mode & 060) == 060) why += ": the user MoonLight runs as must be in group " + node->group + ")";
        else why += ": it grants its group no read and write, so check that piolib's udev rule is installed (see its README))";
    }
    return why;
}

std::string pioClaimFailure(int rc) {
    return std::string("cannot claim a PIO state machine: ") + std::strerror(-rc) +
           " (the RP1 has 4, and another client may hold them all)";
}

std::string pioXferConfigFailure(int rc, uint32_t bufSize, uint32_t bufCount) {
    std::string why = std::string("cannot configure the PIO DMA ring (buf_size=") + std::to_string(bufSize) +
                      " buf_count=" + std::to_string(bufCount) + "): " + std::strerror(-rc);
    if (rc == -EBUSY) why += " (another client holds the DMA)";
    return why;
}

std::string pioProgramFailure(int rc, int programLength) {
    return std::string("cannot load the PIO program: ") + std::strerror(-rc) +
           " (it needs " + std::to_string(programLength) + " of the 32 instruction words)";
}

}  // namespace mm::platform
