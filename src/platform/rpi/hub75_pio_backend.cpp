// SPDX-License-Identifier: GPL-3.0-or-later

/// The `PioBackend` factory: the stub when asked for it, else the rp1 backend, or null when the build has none; and the messages the backends give, as text a host test can read (see hub75_pio_backend.h).

#include "platform/rpi/hub75_pio_backend.h"
#include "platform/rpi/hub75_pio_backend_rp1.h"

#include <atomic>
#include <cerrno>
#include <cstring>

namespace mm::platform {

#ifdef MM_HUB75_PIO_HAVE_RP1
// The real device, defined in hub75_pio_rp1_device.cpp and declared here alone, so this factory is the one way to it.
std::unique_ptr<Rp1PioDevice> createLinuxPio0Device();
#endif

namespace {

// A program's gate is open; the test binary closes it before every case (hub75_pio_backend.h, Real-backend gate).
std::atomic<bool> gateOpen_{true};
std::atomic<uint32_t> realOpens_{0};   // real backends created
std::atomic<uint32_t> gateRefusals_{0};   // real backends the closed gate refused

}  // namespace

PioBackend* createPioBackend(bool preferStub) {
    if (preferStub) return createStubBackend();
    if (!gateOpen_.load()) { gateRefusals_++; return nullptr; }   // refused here, before /dev/pio0 is looked at
#ifdef MM_HUB75_PIO_HAVE_RP1
    realOpens_++;
    return createRp1Backend(createLinuxPio0Device());
#else
    return nullptr;   // no rp1 backend in this build: the engine says why
#endif
}

const char* pioBackendRefusal() {
    return gateOpen_.load() ? kPioNoBackendRefusal : kPioTestRefusal;
}

bool pioBackendGateOpen() { return gateOpen_.load(); }

void setPioBackendGateOpen(bool open) { gateOpen_.store(open); }

uint32_t pioBackendRealOpens() { return realOpens_.load(); }

uint32_t pioBackendGateRefusals() { return gateRefusals_.load(); }

std::string pioModeString(const PioNode& node) {
    std::string out(1, node.characterDevice ? 'c' : '-');
    const char* const rwx = "rwxrwxrwx";
    for (int bit = 8; bit >= 0; bit--) out += (node.mode & (1u << bit)) ? rwx[8 - bit] : '-';
    return out;
}

std::string pioRp1OpenFailure(int err, const PioNode* node) {
    if (err == ENOENT || err == ENODEV || err == ENXIO)
        return "no /dev/pio0 on this system (open: " + std::string(std::strerror(err)) + "): is this a Pi 5 whose kernel has the RP1 PIO driver?";
    std::string why = std::string("cannot open /dev/pio0: ") + std::strerror(err);
    if (err == EACCES && node != nullptr) {
        why += " (the node is " + pioModeString(*node) + " " + node->owner + " " + node->group;
        // The group's access decides: a node that gives its group no read and write is for the owner alone, and no group helps.
        if ((node->mode & 060) == 060) why += ": the user MoonLight runs as must be in group " + node->group + ")";
        else why += ": it grants its group no read and write, so check the udev rule that sets the node's group access)";
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
