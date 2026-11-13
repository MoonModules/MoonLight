// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// What a desktop program says about itself in its first bytes: which system loads it and which processor runs it.
///
/// @technical
///
/// A desktop vets a new version of itself as a board vets an image (`FirmwareImage.h`), by the header its system's loader reads:
///
/// | System | Format | Magic | Processor field |
/// |--------|--------|-------|-----------------|
/// | Linux | ELF (64-bit) | `7F 'E' 'L' 'F'`, class 2 | `e_machine` at 18: 62 x86-64, 183 AArch64 |
/// | macOS | Mach-O (64-bit) | `CF FA ED FE` | `cputype` at 4: `0x01000007` x86-64, `0x0100000C` arm64 |
/// | Windows | PE | `'M' 'Z'`, then `"PE\0\0"` at the offset at `0x3C` | `Machine` after it: `0x8664` x64, `0xAA64` arm64 |
///
/// All three store their fields little-endian on these processors.
///
/// A universal macOS binary is refused, the release shipping one processor per file.

#include <cstddef>
#include <cstdint>
#include <span>

namespace mm {

/// The executable formats a desktop runs.
enum class ProgramSystem : uint8_t { Linux, MacOS, Windows };
/// The processors a desktop runs on.
enum class ProgramCpu : uint8_t { X64, Arm64 };

namespace program_image {
inline uint16_t le16(std::span<const uint8_t> b, size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); }
inline uint32_t le32(std::span<const uint8_t> b, size_t at) {
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) | (static_cast<uint32_t>(b[at + 2]) << 16)
           | (static_cast<uint32_t>(b[at + 3]) << 24);
}

/// What a header says: whether it is a program at all, the system that loads it, and the processor field it names.
struct Header {
    bool program = false;                         ///< a known executable format at all
    ProgramSystem system = ProgramSystem::Linux;  ///< the system whose loader reads it
    bool x64 = false;                             ///< built for x86-64
    bool arm64 = false;                           ///< built for arm64
};

inline Header elf(std::span<const uint8_t> b) {
    if (b.size() < 20 || b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F' || b[4] != 2) return {};
    return {true, ProgramSystem::Linux, le16(b, 18) == 62, le16(b, 18) == 183};
}

inline Header machO(std::span<const uint8_t> b) {
    if (b.size() < 8 || le32(b, 0) != 0xFEEDFACFu) return {};
    return {true, ProgramSystem::MacOS, le32(b, 4) == 0x01000007u, le32(b, 4) == 0x0100000Cu};
}

inline Header pe(std::span<const uint8_t> b) {
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return {};
    const uint32_t peAt = le32(b, 0x3C);
    if (b.size() < static_cast<size_t>(peAt) + 6 || le32(b, peAt) != 0x00004550u) return {};
    return {true, ProgramSystem::Windows, le16(b, peAt + 4) == 0x8664, le16(b, peAt + 4) == 0xAA64};
}
}  // namespace program_image

/// Why `head`, a program's first 4 KB, cannot replace the program of `system` on `cpu`, which the platform names, or nullptr when it can.
inline const char* programRejection(std::span<const uint8_t> head, ProgramSystem system, ProgramCpu cpu) {
    program_image::Header h = program_image::elf(head);
    if (!h.program) h = program_image::machO(head);
    if (!h.program) h = program_image::pe(head);
    if (!h.program) return "not a program";   // a 404 page, an archive, a board's image
    if (h.system != system) return "a program for another system";
    return (cpu == ProgramCpu::Arm64 ? h.arm64 : h.x64) ? nullptr : "a program for another processor";
}

}  // namespace mm
