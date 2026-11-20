// SPDX-License-Identifier: GPL-3.0-or-later

/// The real `Rp1PioDevice`: `open("/dev/pio0")`, `ioctl` and `close`, built where the platform has the Linux headers (hub75_pio_backend_rp1.h).
///
/// It makes one call per ioctl and retries none: a failed call comes back as the negative errno, an interrupted one as -EINTR, and the backend decides what that means.

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "platform/rpi/hub75_pio_backend_rp1.h"
#include "platform/rpi/hub75_pio_rp1_uapi.h"

#ifdef __linux__
// The numbers in hub75_pio_rp1_uapi.h are the ones the kernel's own macros give for the same structs.
namespace rp1 = mm::platform::rp1;
static_assert(rp1::kIocSmXferData32 == _IOW(rp1::kIoctlMagic, 2, rp1::SmXferData32Args), "XFER_DATA32 number");
static_assert(rp1::kIocSmConfigXfer32 == _IOW(rp1::kIoctlMagic, 3, rp1::SmConfigXfer32Args), "CONFIG_XFER32 number");
static_assert(rp1::kIocAddProgram == _IOW(rp1::kIoctlMagic, 11, rp1::AddProgramArgs), "ADD_PROGRAM number");
static_assert(rp1::kIocRemoveProgram == _IOW(rp1::kIoctlMagic, 12, rp1::RemoveProgramArgs), "REMOVE_PROGRAM number");
static_assert(rp1::kIocSmClaim == _IOW(rp1::kIoctlMagic, 20, rp1::SmClaimArgs), "SM_CLAIM number");
static_assert(rp1::kIocSmInit == _IOW(rp1::kIoctlMagic, 30, rp1::SmInitArgs), "SM_INIT number");
static_assert(rp1::kIocSmClearFifos == _IOW(rp1::kIoctlMagic, 33, rp1::SmClearFifosArgs), "SM_CLEAR_FIFOS number");
static_assert(rp1::kIocSmSetPins == _IOW(rp1::kIoctlMagic, 35, rp1::SmSetPinsArgs), "SM_SET_PINS number");
static_assert(rp1::kIocSmSetPindirs == _IOW(rp1::kIoctlMagic, 36, rp1::SmSetPindirsArgs), "SM_SET_PINDIRS number");
static_assert(rp1::kIocSmSetEnabled == _IOW(rp1::kIoctlMagic, 37, rp1::SmSetEnabledArgs), "SM_SET_ENABLED number");
static_assert(rp1::kIocSmFifoState == _IOWR(rp1::kIoctlMagic, 44, rp1::SmFifoStateArgs), "SM_FIFO_STATE number");
static_assert(rp1::kIocGpioSetFunction == _IOW(rp1::kIoctlMagic, 51, rp1::GpioSetFunctionArgs), "GPIO_SET_FUNCTION number");
#endif

namespace mm::platform {

namespace {

constexpr const char* kPioDevice = "/dev/pio0";

class LinuxPio0Device : public Rp1PioDevice {
  public:
    ~LinuxPio0Device() override { close(); }

    int open() override {
        const int opened = ::open(kPioDevice, O_RDWR | O_CLOEXEC);
        if (opened < 0) return errno;
        fd_.store(opened);
        return 0;
    }

    int sendIoctl(unsigned long request, void* arg, size_t /*argBytes*/) override {
        const int rc = ::ioctl(fd_.load(), request, arg);
        return rc < 0 ? -errno : rc;
    }

    void close() override {
        const int fd = fd_.exchange(-1);
        if (fd >= 0) ::close(fd);
    }

    bool node(PioNode& out) const override {
        struct stat st;
        if (stat(kPioDevice, &st) != 0) return false;
        out.characterDevice = S_ISCHR(st.st_mode);
        out.mode = st.st_mode & 07777;
        // The reentrant lookups, since the seam can be reached from any task, with a buffer on the heap so the frame stays small: this runs on a failed open only.
        std::vector<char> buf(512);
        passwd user, *userFound = nullptr;
        group grp, *grpFound = nullptr;
        out.owner = getpwuid_r(st.st_uid, &user, buf.data(), buf.size(), &userFound) == 0 && userFound ? user.pw_name : std::to_string(st.st_uid);
        out.group = getgrgid_r(st.st_gid, &grp, buf.data(), buf.size(), &grpFound) == 0 && grpFound ? grp.gr_name : std::to_string(st.st_gid);
        return true;
    }

  private:
    std::atomic<int> fd_{-1};   // written by open and close, read by the ioctls of two threads
};

}  // namespace

// Declared in hub75_pio_backend.cpp, the one caller.
std::unique_ptr<Rp1PioDevice> createLinuxPio0Device() { return std::make_unique<LinuxPio0Device>(); }

}  // namespace mm::platform
