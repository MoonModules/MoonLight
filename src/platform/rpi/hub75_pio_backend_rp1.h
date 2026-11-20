// SPDX-License-Identifier: GPL-3.0-or-later

/// The device seam under the rp1 PIO backend: `Rp1PioDevice` is the one place that opens `/dev/pio0`, issues its ioctls and closes it, so the backend's logic builds and runs on any host against a fake kernel (test/unit/core/fake_rp1_pio0.h).
///
/// The backend itself is hub75_pio_backend_rp1.cpp, which speaks the ioctls declared in hub75_pio_rp1_uapi.h and implements `PioBackend` (hub75_pio_backend.h). The real device is Linux's (hub75_pio_rp1_device.cpp), built wherever the platform has the Linux headers, and only createPioBackend makes it.
///
/// @technical
///
/// ## The device seam
///
/// An ioctl result follows the kernel's: 0 or a positive value on success, and the negative errno on failure, so a backend reads no `errno`. The caller also passes how many bytes it owns at the argument, because released kernels copy a positive result back over the argument and a fake detects an overrun by that count.
///
/// The seam makes no retry: an interrupted call comes back as -EINTR with the argument possibly rewritten by the kernel, and the caller builds a fresh one before any later call. Two threads may call `sendIoctl` at once, the refresh thread inside a transfer and the closing thread inside the stop.

#pragma once

#include <cstddef>
#include <memory>

#include "platform/rpi/hub75_pio_backend.h"

namespace mm::platform {

/// One open file on /dev/pio0: the kernel keeps its claims, program references and DMA channels per open file, and releases them all when the file closes.
class Rp1PioDevice {
  public:
    /// Lets the backend delete a device through the interface.
    virtual ~Rp1PioDevice() = default;

    /// Opens the device node; returns 0, or the positive errno.
    virtual int open() = 0;

    /// Issues one ioctl; `arg` points at `argBytes` bytes the caller owns. Returns the kernel's result (0 or more) or the negative errno, and may run on two threads at once (@xref{the-device-seam|the device seam}).
    virtual int sendIoctl(unsigned long request, void* arg, size_t argBytes) = 0;

    /// Closes the device; the kernel then releases every state machine, program reference and DMA channel this open held.
    virtual void close() = 0;

    /// Reads the node's type, mode, owner and group for the permission message; false when there is no node.
    virtual bool node(PioNode& node) const = 0;
};

/// Creates the rp1 backend on `device`, which the caller keeps alive as long as the backend; a test passes its fake kernel.
PioBackend* createRp1Backend(Rp1PioDevice& device);

/// Creates the rp1 backend on `device` and owns it; createPioBackend passes the real device here (hub75_pio_backend.cpp), so every real open meets the gate.
PioBackend* createRp1Backend(std::unique_ptr<Rp1PioDevice> device);

}  // namespace mm::platform
