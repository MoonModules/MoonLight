/// A fake `/dev/pio0` for the rp1 backend's tests: an `Rp1PioDevice` that answers the ioctls as the RP1 PIO driver does, from its sources (hub75_pio_backend.h, Submit semantics), so the backend's logic runs on any host with no Pi attached.
///
/// It models the driver's dispatch (an unknown command is -EOPNOTSUPP, a wrong size -EINVAL, a positive result is copied back as `ret` bytes, as released kernels do; the 6.18 head copies at most the argument's size), the claims, the instruction memory, the state machines, the DMA rings, the TX FIFO state, and a close that releases all the open held. A transfer returns once queued, as on 6.18.
///
/// A test scripts the faults: an errno for any command, a transfer that hangs until released, a transfer a signal interrupts, a ring or a state machine another client holds. Where the contract leaves the firmware's answer open (the errno for a taken state machine), the fake gives a stated guess, and the bench measures it.

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/rpi/hub75_pio_backend_rp1.h"
#include "platform/rpi/hub75_pio_rp1_uapi.h"

namespace mm {

namespace rp1 = mm::platform::rp1;

/// One transfer the fake received, as the driver saw its argument.
struct FakeTransfer {
    uint16_t sm = 0;               ///< the state machine the transfer named
    uint16_t dir = 0;              ///< the direction the transfer named, 0 toward the state machine
    uint32_t bytes = 0;            ///< the byte count the transfer asked for
    uint64_t dataPointer = 0;      ///< the `data` field as received
    uint32_t chunks = 0;           ///< the ring buffers the frame fills, `bytes` over the buffer size rounded up
    std::vector<uint8_t> data;     ///< a copy of the frame's bytes
};

/// The fake kernel: one open file on /dev/pio0 over the state the driver keeps, with the script and the inspection calls below.
class FakeRp1Pio0 : public mm::platform::Rp1PioDevice {
  public:
    // ---- the device seam ----

    /// Opens the fake: succeeds unless a script made it fail.
    int open() override {
        std::lock_guard<std::mutex> lock(mutex_);
        opens_++;
        if (openErrno_ != 0) return openErrno_;
        open_ = true;   // a second open succeeds: the driver has no exclusivity
        return 0;
    }

    /// Answers one ioctl as the driver's dispatcher does.
    int sendIoctl(unsigned long request, void* arg, size_t argBytes) override {
        std::unique_lock<std::mutex> lock(mutex_);
        const uint32_t command = rp1::ioctlCommand(request);
        const size_t size = rp1::ioctlArgBytes(request);
        requests_[command] = request;
        if (command == rp1::ioctlCommand(rp1::kIocSmClaim)) claimArgBytes_ = std::max(claimArgBytes_, argBytes);
        if (!open_) {
            record("closed:" + (known(command) ? nameOf(command, static_cast<const uint8_t*>(arg)) : std::string("unknown")), -1);   // a call on a closed device shows in the log
            return -EBADF;
        }
        if (!known(command)) {
            record("unknown", -1);
            return -EOPNOTSUPP;
        }
        if (size != sizeOf(command)) {
            record("wrong_size", -1);
            return -EINVAL;
        }
        if (argBytes < size) return -EFAULT;   // the caller owns fewer bytes than the request declares
        std::vector<uint8_t> kernelCopy(static_cast<const uint8_t*>(arg), static_cast<const uint8_t*>(arg) + size);
        bool writeBack = false;
        const int rc = dispatch(command, kernelCopy.data(), writeBack, lock);
        uint8_t* user = static_cast<uint8_t*>(arg);
        if (writeBack) std::memcpy(user, kernelCopy.data(), size);
        if (rc > 0) {
            // The driver copies a positive result back as that many bytes, past the argument's own size on released kernels: what lies beyond is stack garbage, and a buffer too small for it is overrun.
            const size_t copied = static_cast<size_t>(rc);
            if (copied > argBytes) overruns_++;
            for (size_t i = 0; i < std::min(copied, argBytes); i++) user[i] = i < size ? kernelCopy[i] : 0xEE;
        }
        return rc;
    }

    /// Closes the open file and releases what it held.
    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        closes_++;
        // The driver disables and unclaims every state machine this open claimed, frees its DMA channels and drops its program references; pin values and functions stay as they are.
        for (int sm = 0; sm < rp1::kSmCount; sm++) {
            if (!(clientClaims_ & (1u << sm))) continue;
            sms_[sm].enabled = false;
            claimed_ &= ~(1u << sm);
        }
        clientClaims_ = 0;
        for (auto& channels : dmas_) {
            for (Dma& dma : channels)
                if (dma.heldByClient) dma = Dma{dma.heldByOther, false, 0, 0};
        }
        for (Program& program : programs_) {
            if (program.heldByClient) program.refs--;
            program.heldByClient = false;
        }
        programs_.erase(std::remove_if(programs_.begin(), programs_.end(), [](const Program& p) { return p.refs <= 0; }), programs_.end());
        open_ = false;
    }

    /// Gives the node a script set, or false.
    bool node(mm::platform::PioNode& out) const override {
        if (!nodeInfo_) return false;
        out = *nodeInfo_;
        return true;
    }

    // ---- the script ----

    /// Makes the next opens fail with the positive errno `err`, or succeed again with 0.
    void failOpen(int err) { openErrno_ = err; }
    /// The node /dev/pio0 reads as, for the permission message; none by default.
    void setNode(const mm::platform::PioNode& node) { nodeInfo_ = node; }
    /// Makes every ioctl of `command` answer the negative errno `err` with no effect, as a firmware refusal does; 0 clears it.
    void failCommand(uint32_t command, int err) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (err == 0) failures_.erase(command);
        else failures_[command] = err;
    }
    /// Marks state machine `sm` as claimed by another client, so a claim for any free one skips it.
    void otherClientClaims(int sm) { claimed_ |= 1u << sm; }
    /// Marks state machine `sm`'s TX DMA channel as held by another client, so configuring it answers -EBUSY.
    void otherClientHoldsDma(int sm) { dmas_[sm][rp1::kDirToSm].heldByOther = true; }
    /// Loads a program of `length` words as another client, at the first free offset, so a backend's program lands after it.
    void otherClientLoadsProgram(int length) {
        Program program;
        for (const Program& p : programs_) program.offset = std::max(program.offset, p.offset + p.length);
        program.length = length;
        program.refs = 1;
        program.words.assign(static_cast<size_t>(length), 0xFFFF);
        programs_.push_back(program);
    }
    /// Sets what the TX FIFO state reads: empty or holding words.
    void setTxFifoEmpty(bool empty) { txEmpty_ = empty; }
    /// Makes transfers block inside the call until releaseTransfers(), as a submit stuck in the kernel does.
    void hangTransfers() {
        std::lock_guard<std::mutex> lock(mutex_);
        hang_ = true;
    }
    /// Lets the blocked transfers go, each returning `result`.
    void releaseTransfers(int result = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        hang_ = false;
        hangResult_ = result;
        released_.notify_all();
    }
    /// Makes the next `count` transfers end as a signal ends them on 6.18.53 and later: the argument rewritten in place with the data advanced and the byte count reduced, and -EINTR returned.
    void interruptTransfers(int count) {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupts_ = count;
    }

    // ---- what the backend left ----

    /// Every ioctl in order by a short name, with `set_enabled` split into `enable` and `disable`; a run of one name is shown once when `collapse` is set.
    std::vector<std::string> calls(bool collapse = false) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        for (const std::string& call : calls_)
            if (!collapse || out.empty() || out.back() != call) out.push_back(call);
        return out;
    }
    /// The state machine each ioctl of the named kind named, in order.
    std::vector<int> smsOf(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int> out;
        for (size_t i = 0; i < callSms_.size(); i++)
            if (calls_[i] == name) out.push_back(callSms_[i]);
        return out;
    }
    /// The words of the program loaded at index `index` in load order.
    std::vector<uint16_t> programWords(size_t index) const { return programs_.at(index).words; }
    /// How many ioctls of the named kind came in.
    int count(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count(calls_.begin(), calls_.end(), name));
    }
    /// The last request number the fake received for `command`, 0 when there was none.
    unsigned long lastRequest(uint32_t command) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = requests_.find(command);
        return found == requests_.end() ? 0 : found->second;
    }
    /// The opens attempted.
    int opens() const { return opens_; }
    /// The closes that released an open file.
    int closes() const { return closes_; }
    /// Whether a file is open now.
    bool isOpen() const { return open_; }
    /// The bytes the caller owned at the claim's argument, the most it passed.
    size_t claimArgBytes() const { return claimArgBytes_; }
    /// The positive results copied back over an argument too small for them.
    int overruns() const { return overruns_; }
    /// The state machines claimed, by every client.
    unsigned claimedMask() const { return claimed_; }
    /// The state machines this open claimed and has not released.
    unsigned clientClaimedMask() const { return clientClaims_; }
    /// Whether state machine `sm` is enabled.
    bool smEnabled(int sm) const { return sms_[sm].enabled; }
    /// The four config registers the last init gave state machine `sm`.
    rp1::SmConfig smConfig(int sm) const { return sms_[sm].config; }
    /// The program counter the last init of `sm` started at, or -1.
    int smInitialPc(int sm) const { return sms_[sm].initialPc; }
    /// Whether `sm` has been initialized.
    bool smInitialized(int sm) const { return sms_[sm].initialized; }
    /// The pin values after every write, bit n being GPIO n.
    uint32_t pinValues() const { return pinValues_; }
    /// The pin directions after every write, a 1 for an output.
    uint32_t pinDirections() const { return pinDirs_; }
    /// The function a GPIO was handed to, 0 for a GPIO no call named.
    uint16_t gpioFunction(int gpio) const { return gpioFunctions_[gpio]; }
    /// Every pin write as (values, mask), in order.
    std::vector<std::pair<uint32_t, uint32_t>> pinWrites() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pinWrites_;
    }
    /// The words of instruction memory in use.
    int programWordsUsed() const {
        int used = 0;
        for (const Program& p : programs_) used += p.length;
        return used;
    }
    /// The load offsets of the programs in instruction memory, in load order.
    std::vector<int> programOffsets() const {
        std::vector<int> offsets;
        for (const Program& p : programs_) offsets.push_back(p.offset);
        return offsets;
    }
    /// Whether `sm`'s TX ring is configured and held by this open.
    bool dmaConfigured(int sm) const { return dmas_[sm][rp1::kDirToSm].configured; }
    /// The ring's buffer size in bytes.
    uint32_t dmaBufSize(int sm) const { return dmas_[sm][rp1::kDirToSm].bufSize; }
    /// The ring's buffer count.
    uint32_t dmaBufCount(int sm) const { return dmas_[sm][rp1::kDirToSm].bufCount; }
    /// Every transfer received, in order.
    std::vector<FakeTransfer> transfers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return transfers_;
    }
    /// The argument the last interrupted transfer was rewritten to: the data advanced and the byte count reduced.
    rp1::SmXferData32Args lastRewrite() const { return lastRewrite_; }
    /// The FIFO state requests received.
    int fifoPolls() const { return count("fifo_state"); }
    /// The `tx` byte of the last FIFO state request, or -1.
    int lastFifoTx() const { return lastFifoTx_; }
    /// Waits until `count` transfers are blocked inside the call, or the timeout passes.
    bool waitForBlockedTransfers(int count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return blockedChanged_.wait_for(lock, timeout, [&] { return blocked_ >= count; });
    }

  private:
    struct Sm {
        bool enabled = false;
        bool initialized = false;
        int initialPc = -1;
        rp1::SmConfig config{};
    };
    struct Dma {
        bool heldByOther = false;
        bool heldByClient = false;
        uint32_t bufSize = 0;
        uint32_t bufCount = 0;
        bool configured = false;
        Dma() = default;
        Dma(bool other, bool client, uint32_t size, uint32_t count) : heldByOther(other), heldByClient(client), bufSize(size), bufCount(count) {}
    };
    struct Program {
        int offset = 0;
        int length = 0;
        int refs = 0;
        bool heldByClient = false;
        std::vector<uint16_t> words;
    };

    static bool known(uint32_t command) {
        for (const unsigned long request : kTable)
            if (rp1::ioctlCommand(request) == command) return true;
        return false;
    }
    static size_t sizeOf(uint32_t command) {
        for (const unsigned long request : kTable)
            if (rp1::ioctlCommand(request) == command) return rp1::ioctlArgBytes(request);
        return 0;
    }
    template <class Args>
    static Args read(const uint8_t* kernelCopy) {
        Args args;
        std::memcpy(&args, kernelCopy, sizeof(Args));
        return args;
    }

    int dispatch(uint32_t command, uint8_t* kernelCopy, bool& writeBack, std::unique_lock<std::mutex>& lock) {
        // The log name first: a call is recorded whether the firmware refuses it or not.
        record(nameOf(command, kernelCopy), smOf(command, kernelCopy));
        const auto refused = failures_.find(command);
        if (refused != failures_.end()) return refused->second;
        if (command == rp1::ioctlCommand(rp1::kIocSmClaim)) return claim(read<rp1::SmClaimArgs>(kernelCopy));
        if (command == rp1::ioctlCommand(rp1::kIocAddProgram)) return addProgram(read<rp1::AddProgramArgs>(kernelCopy));
        if (command == rp1::ioctlCommand(rp1::kIocRemoveProgram)) return removeProgram(read<rp1::RemoveProgramArgs>(kernelCopy));
        if (command == rp1::ioctlCommand(rp1::kIocSmInit)) return init(read<rp1::SmInitArgs>(kernelCopy));
        if (command == rp1::ioctlCommand(rp1::kIocSmSetPins)) return setPins(read<rp1::SmSetPinsArgs>(kernelCopy));
        if (command == rp1::ioctlCommand(rp1::kIocSmSetPindirs)) return setPindirs(read<rp1::SmSetPindirsArgs>(kernelCopy));
        if (command == rp1::ioctlCommand(rp1::kIocGpioSetFunction)) return setFunction(read<rp1::GpioSetFunctionArgs>(kernelCopy));
        if (command == rp1::ioctlCommand(rp1::kIocSmSetEnabled)) return setEnabled(read<rp1::SmSetEnabledArgs>(kernelCopy));
        if (command == rp1::ioctlCommand(rp1::kIocSmClearFifos)) return 0;
        if (command == rp1::ioctlCommand(rp1::kIocSmFifoState)) return fifoState(kernelCopy);
        if (command == rp1::ioctlCommand(rp1::kIocSmConfigXfer32)) return configXfer(read<rp1::SmConfigXfer32Args>(kernelCopy));
        return transfer(kernelCopy, writeBack, lock);
    }

    void record(const std::string& name, int sm) {
        calls_.push_back(name);
        callSms_.push_back(sm);
    }

    /// The state machine a call names, from its argument's first field, or -1 for a call that names SMs another way (a claim's or an enable's mask) or none.
    static int smOf(uint32_t command, const uint8_t* kernelCopy) {
        for (const unsigned long request : {rp1::kIocSmXferData32, rp1::kIocSmConfigXfer32, rp1::kIocSmInit, rp1::kIocSmClearFifos, rp1::kIocSmSetPins, rp1::kIocSmSetPindirs, rp1::kIocSmFifoState}) {
            if (rp1::ioctlCommand(request) != command) continue;
            uint16_t sm;
            std::memcpy(&sm, kernelCopy, sizeof(sm));
            return sm;
        }
        return -1;
    }

    static std::string nameOf(uint32_t command, const uint8_t* kernelCopy) {
        if (command == rp1::ioctlCommand(rp1::kIocSmClaim)) return "claim";
        if (command == rp1::ioctlCommand(rp1::kIocAddProgram)) return "add_program";
        if (command == rp1::ioctlCommand(rp1::kIocRemoveProgram)) return "remove_program";
        if (command == rp1::ioctlCommand(rp1::kIocSmInit)) return "init";
        if (command == rp1::ioctlCommand(rp1::kIocSmSetPins)) return "set_pins";
        if (command == rp1::ioctlCommand(rp1::kIocSmSetPindirs)) return "set_pindirs";
        if (command == rp1::ioctlCommand(rp1::kIocGpioSetFunction)) return "gpio_function";
        if (command == rp1::ioctlCommand(rp1::kIocSmSetEnabled)) return read<rp1::SmSetEnabledArgs>(kernelCopy).enable ? "enable" : "disable";
        if (command == rp1::ioctlCommand(rp1::kIocSmClearFifos)) return "clear_fifos";
        if (command == rp1::ioctlCommand(rp1::kIocSmFifoState)) return "fifo_state";
        if (command == rp1::ioctlCommand(rp1::kIocSmConfigXfer32)) return "config_xfer";
        return "xfer";
    }

    /// A mask of 0 claims any free state machine and returns its index; a claim when no state machine is free is refused with -EBUSY, a guess, since the firmware's errno for it is not in the kernel source.
    int claim(const rp1::SmClaimArgs& args) {
        if (args.mask != 0) return -EINVAL;   // the backend asks for any free state machine
        for (int sm = 0; sm < rp1::kSmCount; sm++) {
            if (claimed_ & (1u << sm)) continue;
            claimed_ |= 1u << sm;
            clientClaims_ |= 1u << sm;
            return sm;
        }
        return -EBUSY;
    }

    /// The same program loaded twice shares its slots and counts a reference; a new one takes its fixed origin or the first run of free words, or answers -ENOSPC, a guess like the claim's.
    int addProgram(const rp1::AddProgramArgs& args) {
        if (args.numInstrs < 1 || args.numInstrs > rp1::kInstructionCount) return -EINVAL;
        const std::vector<uint16_t> words(args.instrs, args.instrs + args.numInstrs);
        for (Program& program : programs_) {
            if (program.words != words) continue;
            program.refs++;
            program.heldByClient = true;
            return program.offset;
        }
        const bool fixed = args.origin != rp1::kOriginAny;
        if (fixed && args.origin + args.numInstrs > rp1::kInstructionCount) return -EINVAL;
        for (int offset = fixed ? args.origin : 0; offset + args.numInstrs <= rp1::kInstructionCount; offset++) {
            if (fixed && offset != args.origin) break;   // a fixed origin loads there or not at all
            bool free = true;
            for (const Program& p : programs_) free = free && (offset + args.numInstrs <= p.offset || offset >= p.offset + p.length);
            if (!free) continue;
            Program program;
            program.offset = offset;
            program.length = args.numInstrs;
            program.refs = 1;
            program.heldByClient = true;
            program.words = words;
            programs_.push_back(program);
            return offset;
        }
        return -ENOSPC;
    }

    /// Removing a program this open holds drops the open's hold and the reference; one it does not hold is -ENODEV, as on 6.18.
    int removeProgram(const rp1::RemoveProgramArgs& args) {
        for (auto it = programs_.begin(); it != programs_.end(); ++it) {
            if (it->offset != args.origin || it->length != args.numInstrs || !it->heldByClient) continue;
            it->heldByClient = false;
            if (--it->refs <= 0) programs_.erase(it);
            return 0;
        }
        return -ENODEV;
    }

    int init(const rp1::SmInitArgs& args) {
        if (args.sm >= rp1::kSmCount || args.initialPc >= rp1::kInstructionCount) return -EINVAL;
        Sm& sm = sms_[args.sm];
        sm.config = args.config;
        sm.initialPc = args.initialPc;
        sm.initialized = true;
        sm.enabled = false;   // an init leaves the state machine disabled
        return 0;
    }

    int setPins(const rp1::SmSetPinsArgs& args) {
        if (args.sm >= rp1::kSmCount) return -EINVAL;
        pinValues_ = (pinValues_ & ~args.mask) | (args.values & args.mask);
        pinWrites_.emplace_back(args.values, args.mask);
        return 0;
    }

    int setPindirs(const rp1::SmSetPindirsArgs& args) {
        if (args.sm >= rp1::kSmCount) return -EINVAL;
        pinDirs_ = (pinDirs_ & ~args.mask) | (args.dirs & args.mask);
        return 0;
    }

    int setFunction(const rp1::GpioSetFunctionArgs& args) {
        if (args.gpio >= 28 || args.function > 0x1F) return -EINVAL;
        gpioFunctions_[args.gpio] = args.function;
        return 0;
    }

    int setEnabled(const rp1::SmSetEnabledArgs& args) {
        if (args.mask > 0xF) return -EINVAL;
        for (int sm = 0; sm < rp1::kSmCount; sm++)
            if (args.mask & (1u << sm)) sms_[sm].enabled = args.enable != 0;
        return 0;
    }

    /// Returns 8, the bytes it wrote back, as the driver does, and the dispatcher copies them over the argument.
    int fifoState(uint8_t* kernelCopy) {
        rp1::SmFifoStateArgs args = read<rp1::SmFifoStateArgs>(kernelCopy);
        lastFifoTx_ = args.tx;
        args.level = txEmpty_ ? 0 : 8;
        args.empty = txEmpty_ ? 1 : 0;
        args.full = txEmpty_ ? 0 : 1;
        std::memcpy(kernelCopy, &args, sizeof(args));
        return static_cast<int>(sizeof(args));
    }

    /// A size a multiple of 4 and 1 to 4 buffers, a channel another client holds is -EBUSY, and a reconfigure by the holder replaces its ring.
    int configXfer(const rp1::SmConfigXfer32Args& args) {
        if (args.sm >= rp1::kSmCount || args.dir >= 2) return -EINVAL;
        if (args.bufSize == 0 || args.bufSize % 4 != 0 || args.bufCount < 1 || args.bufCount > 4) return -EINVAL;
        Dma& dma = dmas_[args.sm][args.dir];
        if (dma.heldByOther) return -EBUSY;
        dma.heldByClient = true;
        dma.configured = true;
        dma.bufSize = args.bufSize;
        dma.bufCount = args.bufCount;
        return 0;
    }

    /// A transfer on a configured channel is cut into ring buffers and returns once queued. It records the argument as received; it hangs when scripted to, and ends -EINTR with the argument rewritten when a signal is scripted.
    int transfer(uint8_t* kernelCopy, bool& writeBack, std::unique_lock<std::mutex>& lock) {
        rp1::SmXferData32Args args = read<rp1::SmXferData32Args>(kernelCopy);
        if (args.sm >= rp1::kSmCount || args.dir >= 2 || args.dataBytes == 0 || args.data == 0) return -EINVAL;
        const Dma& dma = dmas_[args.sm][args.dir];   // a transfer on a direction nobody configured is -EINVAL
        if (!dma.configured || !dma.heldByClient) return -EINVAL;
        FakeTransfer received;
        received.sm = args.sm;
        received.dir = args.dir;
        received.bytes = args.dataBytes;
        received.dataPointer = args.data;
        received.chunks = (args.dataBytes + dma.bufSize - 1) / dma.bufSize;
        const uint8_t* source = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(args.data));
        received.data.assign(source, source + args.dataBytes);
        transfers_.push_back(received);
        if (interrupts_ > 0) {
            interrupts_--;
            const uint32_t sent = std::max<uint32_t>(4, (args.dataBytes / 2) & ~3u);
            args.data += sent;
            args.dataBytes -= std::min(sent, args.dataBytes);
            std::memcpy(kernelCopy, &args, sizeof(args));
            lastRewrite_ = args;
            writeBack = true;
            return -EINTR;
        }
        if (hang_) {
            blocked_++;
            blockedChanged_.notify_all();
            released_.wait(lock, [&] { return !hang_; });
            blocked_--;
            return hangResult_;
        }
        return 0;
    }

    static constexpr unsigned long kTable[] = {
        rp1::kIocSmXferData32, rp1::kIocSmConfigXfer32, rp1::kIocAddProgram, rp1::kIocRemoveProgram, rp1::kIocSmClaim, rp1::kIocSmInit,
        rp1::kIocSmClearFifos, rp1::kIocSmSetPins, rp1::kIocSmSetPindirs, rp1::kIocSmSetEnabled, rp1::kIocSmFifoState, rp1::kIocGpioSetFunction};

    mutable std::mutex mutex_;
    std::condition_variable released_;
    std::condition_variable blockedChanged_;
    bool open_ = false;
    int openErrno_ = 0;
    int opens_ = 0;
    int closes_ = 0;
    std::optional<mm::platform::PioNode> nodeInfo_;
    std::map<uint32_t, int> failures_;
    std::map<uint32_t, unsigned long> requests_;
    std::vector<std::string> calls_;
    std::vector<int> callSms_;   // the state machine each call in calls_ named, -1 for none
    unsigned claimed_ = 0;         // the firmware keeps one claim mask for every client
    unsigned clientClaims_ = 0;    // the claims this open holds
    size_t claimArgBytes_ = 0;
    int overruns_ = 0;
    Sm sms_[rp1::kSmCount];
    Dma dmas_[rp1::kSmCount][2];   // per state machine and direction, as the driver keys its channels
    std::vector<Program> programs_;
    uint32_t pinValues_ = 0;
    uint32_t pinDirs_ = 0;
    uint16_t gpioFunctions_[28] = {};
    std::vector<std::pair<uint32_t, uint32_t>> pinWrites_;
    bool txEmpty_ = true;
    int lastFifoTx_ = -1;
    bool hang_ = false;
    int hangResult_ = 0;
    int blocked_ = 0;
    int interrupts_ = 0;
    rp1::SmXferData32Args lastRewrite_{};
    std::vector<FakeTransfer> transfers_;
};

}  // namespace mm
