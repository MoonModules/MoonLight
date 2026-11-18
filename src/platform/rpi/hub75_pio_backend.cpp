// SPDX-License-Identifier: GPL-3.0-or-later

/// The `PioBackend` factory: the stub when asked for it, else the piolib backend, or null when the build has no piolib; and the messages the piolib backend gives, as text a host test can read (see hub75_pio_backend.h).

#include "platform/rpi/hub75_pio_backend.h"

#include <atomic>
#include <cerrno>
#include <cstring>

namespace mm::platform {

namespace {
std::atomic<bool> realAllowed_{true};         // a program may open the real backend; the test binary turns it off
std::atomic<uint32_t> realOpens_{0};          // real backends created
}  // namespace

PioBackend* createPioBackend(bool preferStub) {
    if (preferStub) return createStubBackend();
    if (!realAllowed_.load()) return nullptr;   // refused here, before piolib or /dev/pio0 is looked at
#ifdef MM_HUB75_PIO_HAVE_PIOLIB
    realOpens_++;
    return createPiolibBackend();
#else
    return nullptr;   // no piolib in this build: the engine says so
#endif
}

const char* pioBackendRefusal() {
    return realAllowed_.load() ? "needs a Raspberry Pi build with piolib" : "the real PIO is not opened from a test binary";
}

bool pioBackendRealAllowed() { return realAllowed_.load(); }

void setPioBackendRealAllowed(bool allowed) { realAllowed_.store(allowed); }

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
