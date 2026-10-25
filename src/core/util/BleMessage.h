#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm {

/// One newline-delimited request across ATT writes; a held request applies backpressure and an invalid line drains through its delimiter.
class BleMessage {
public:
    /// Maximum request length before its line delimiter.
    static constexpr size_t kCapacity = 512;
    enum class Result : uint8_t { Partial, Ready, Busy, Invalid };

    /// Append an ATT fragment, retaining a complete request until taken.
    Result feed(const uint8_t* bytes, size_t count) {
        if (ready_) return Result::Busy;
        if (!bytes || count == 0) return Result::Invalid;
        // Reject a batch before accepting any of it.
        if (containsBatch(bytes, count)) { reset(); return Result::Invalid; }
        bool invalid = false;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t byte = bytes[i];
            if (byte == '\n') return finishLine();
            if (!appendByte(byte)) invalid = true;
        }
        return invalid ? Result::Invalid : Result::Partial;
    }

    /// Copy and consume one complete request when the destination fits.
    size_t take(char* out, size_t capacity) {
        if (!ready_ || !out || capacity <= used_) return 0;
        const size_t count = used_;
        std::memcpy(out, data_, count + 1);
        reset();
        return count;
    }

    /// Discard partial input, a held request and overflow state.
    void reset() { used_ = 0; ready_ = false; discard_ = false; data_[0] = '\0'; }

private:
    static bool containsBatch(const uint8_t* bytes, size_t count) {
        for (size_t i = 0; i + 1 < count; ++i)
            if (bytes[i] == '\n') return true;
        return false;
    }
    Result finishLine() {
        if (discard_ || used_ == 0) { reset(); return Result::Invalid; }
        if (data_[used_ - 1] == '\r') --used_;
        data_[used_] = '\0';
        ready_ = used_ != 0;
        return ready_ ? Result::Ready : Result::Invalid;
    }
    bool appendByte(uint8_t byte) {
        if (discard_) return false;
        if (byte == 0 || used_ == kCapacity) { used_ = 0; discard_ = true; return false; }
        data_[used_++] = static_cast<char>(byte);
        return true;
    }
    char data_[kCapacity + 1] = {};
    size_t used_ = 0;
    bool ready_ = false;
    bool discard_ = false;
};

}  // namespace mm
