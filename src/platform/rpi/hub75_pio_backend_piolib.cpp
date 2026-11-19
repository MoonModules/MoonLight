/// PioBackend on Raspberry Pi's piolib, built only when piolib is available (see the root CMakeLists.txt); the engine links against piolib, which is not vendored here.
///
/// piolib is BSD-3-Clause, Copyright (c) 2023 Raspberry Pi Ltd (raspberrypi/utils, piolib/).
///
/// The piolib calls go through hub75_pio_c_api.* (compiled as C) because piolib.h is C-only and does not compile as C++.
///
/// The API notes, verified against raspberrypi/utils master (piolib.h and pio_rp1.c), are in @xref{piolib-api-notes|piolib API notes}.
///
/// @technical
///
/// ## Piolib API notes
///
/// - `pio0` is pio_open_helper(0) and calls exit(1) if /dev/pio0 cannot be opened, so the C shim calls pio_open(0), which returns the error, once, and keeps the handle: a second pio_open(0) in the process returns -EBUSY. A missing node shows up as -EINVAL, since piolib then finds no instance.
/// - pio_enable_fatal_errors(pio, false) turns piolib's error paths (e.g. pio_add_program with no program space) into return values plus pio_get_error(), instead of aborting the process. The pinned piolib's rp1_ioctl also returns -ETIMEDOUT and -EREMOTEIO, which stock piolib answers with exit(1) (hub75_pio_backend.h, Submit semantics).
/// - pio_sm_put() stays off the hot path: each put is a mailbox RPC (>= 10 us), far too slow to stream pixels.
/// - Frames go through pio_sm_xfer_data, whose blocking depends on the kernel; see hub75_pio_backend.h.
///
/// ## The device node
///
/// On the bench Pi, /dev/pio0 reads `crw-rw---- root gpio`: the group owns it, so the user MoonLight runs as must be in group `gpio`. A failed open prints the node's actual mode, owner and group, so the message names the group the machine uses, not a guess. The open does not pre-check access: a check and the open are two steps, and the open's own errno is the reliable one.

#include "platform/rpi/hub75_pio_backend.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "platform/rpi/hub75_pio_c_api.h"
#include "platform/rpi/hub75_pio_pins.h"

namespace mm::platform {

namespace {

constexpr const char* kPioDevice = "/dev/pio0";

// The node as stat() reads it, or false when there is none.
bool readNode(PioNode& node) {
    struct stat st;
    if (stat(kPioDevice, &st) != 0) return false;
    node.characterDevice = S_ISCHR(st.st_mode);
    node.mode = st.st_mode & 07777;
    // The reentrant lookups, since the seam can be reached from any task, with a buffer on the heap so the frame stays small: this runs on a failed open only.
    std::vector<char> buf(512);
    passwd user, *userFound = nullptr;
    group grp, *grpFound = nullptr;
    node.owner = getpwuid_r(st.st_uid, &user, buf.data(), buf.size(), &userFound) == 0 && userFound ? user.pw_name : std::to_string(st.st_uid);
    node.group = getgrgid_r(st.st_gid, &grp, buf.data(), buf.size(), &grpFound) == 0 && grpFound ? grp.gr_name : std::to_string(st.st_gid);
    return true;
}

/// How long blankAndStop() waits for the TX FIFO to drain after the blank before disabling the SM anyway; the FIFO holds at most 8 words per direction, so any sane pixel clock drains it in microseconds.
constexpr int kFifoDrainTimeoutMs = 100;

class PiolibBackend : public PioBackend {
  public:
    bool open(std::string& error) override {
        int err = 0;
        pio_ = hub75_pio_open(&err);
        if (pio_ == nullptr) {
            PioNode node;
            error = pioOpenFailure(err, readNode(node) ? &node : nullptr);
            return false;
        }
        sm_ = hub75_pio_claim_sm(pio_);
        if (sm_ < 0) {
            error = pioClaimFailure(sm_);
            hub75_pio_release(pio_, -1);
            pio_ = nullptr;
            sm_ = -1;
            return false;
        }
        opened_ = true;
        return true;
    }

    bool start(const PioStartConfig& config, std::string& error) override {
        if (!opened_) {
            error = "piolib backend not opened";
            return false;
        }
        // DMA ring first (piotest.c configures the transfer before loading the program).
        const int ring = hub75_pio_config_xfer(pio_, sm_, config.xferBufSize, config.xferBufCount);
        if (ring != 0) {
            error = pioXferConfigFailure(ring, config.xferBufSize, config.xferBufCount);
            return false;
        }
        offset_ = hub75_pio_add_program(pio_, config.programInstructions, config.programLength);
        if (offset_ < 0) {
            error = pioProgramFailure(offset_, config.programLength);
            return false;
        }
        programInstructions_ = config.programInstructions;
        programLength_ = config.programLength;
        programLoaded_ = true;

        // The configuration is silent (hub75_pio_c_api.h, Silent configuration): a state machine that did not start shows as the first submit failing or blocking.
        const hub75_pio_sm_params smParams = {offset_, config.wrapTarget, config.wrap,
                                              config.clkdivInt, config.clkdivFrac, kBlankWideWord};
        hub75_pio_init_sm(pio_, sm_, &smParams);
        hub75_pio_clear_fifos(pio_, sm_);
        hub75_pio_set_enabled(pio_, sm_, 1);
        started_ = true;
        return true;
    }

    int submitFrame(const void* data, uint32_t bytes) override {
        if (!started_) return -ENODEV;
        // The kernel copies from the buffer; the transfer does not modify it.
        return hub75_pio_xfer(pio_, sm_, const_cast<void*>(data), bytes);
    }

    bool blankAndStop(const void* blankData, uint32_t blankBytes,
                      std::string& error) override {
        bool ok = true;
        if (started_) {
            // 1. Drive the panel dark: submit the blank, which the engine made as deep as the DMA ring, so that its submit returns only after every real chunk has gone out (hub75_pio_backend.h, Submit semantics).
            const int rc = submitFrame(blankData, blankBytes);
            if (rc != 0) {
                error = "blank frame submit failed (" + std::to_string(rc) +
                        "); continuing with shutdown";
                ok = false;
            }
            // 2. Wait for the TX FIFO to drain, so the SM has shifted out the last blank word and is stalled on `pull` with the pins holding the dark word. Only a blank as deep as the ring makes an empty FIFO mean a blank word.
            if (!waitForFifoDrain()) {
                if (error.empty())
                    error = "TX FIFO did not drain within timeout; "
                            "disabling SM anyway";
                ok = false;
            }
            // 3. Blank is established, so it is safe to stop the SM: the pins hold the dark word (PIO output registers keep their values when the SM is disabled).
            hub75_pio_set_enabled(pio_, sm_, 0);
            hub75_pio_clear_fifos(pio_, sm_);
            started_ = false;
        }
        if (programLoaded_) {
            hub75_pio_remove_program(pio_, programInstructions_, programLength_,
                                     offset_);
            programLoaded_ = false;
        }
        return ok;
    }

    void close() override {
        if (started_) {
            // No blank frame is available here, so disable directly; the engine always calls blankAndStop() before close() on the clean path, so this is only the error path.
            hub75_pio_set_enabled(pio_, sm_, 0);
            started_ = false;
        }
        if (programLoaded_) {
            hub75_pio_remove_program(pio_, programInstructions_, programLength_,
                                     offset_);
            programLoaded_ = false;
        }
        if (opened_) {
            hub75_pio_release(pio_, sm_);
            pio_ = nullptr;
            sm_ = -1;
            opened_ = false;
        }
    }

    const char* name() const override { return "piolib (/dev/pio0)"; }

  private:
    // Polls the TX FIFO until it is empty; false when it is still not empty after kFifoDrainTimeoutMs.
    bool waitForFifoDrain() {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kFifoDrainTimeoutMs);
        while (!hub75_pio_tx_fifo_empty(pio_, sm_)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return true;
    }

    void* pio_ = nullptr;
    int sm_ = -1;
    int offset_ = -1;
    const uint16_t* programInstructions_ = nullptr;
    int programLength_ = 0;
    bool opened_ = false;
    bool started_ = false;
    bool programLoaded_ = false;
};

}  // namespace

PioBackend* createPiolibBackend() { return new PiolibBackend(); }

}  // namespace mm::platform
