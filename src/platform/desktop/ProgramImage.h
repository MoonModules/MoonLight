// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// What a desktop program says about itself in its first bytes: which system loads it and which processor runs it.
///
/// @technical
///
/// A desktop vets a new version of itself as a board vets an image (`FirmwareImage.h`), by the header its system's loader reads:
///
/// | System | Format | Magic | Processor field | Executable |
/// |--------|--------|-------|-----------------|------------|
/// | Linux | ELF (64-bit) | `7F 'E' 'L' 'F'`, class 2 | `e_machine` at 18: 62 x86-64, 183 AArch64 | `e_type` at 16: 2 `ET_EXEC`, or 3 `ET_DYN`, a PIE |
/// | macOS | Mach-O (64-bit) | `CF FA ED FE` | `cputype` at 4: `0x01000007` x86-64, `0x0100000C` arm64 | `filetype` at 12: 2 `MH_EXECUTE` |
/// | Windows | PE | `'M' 'Z'`, then `"PE\0\0"` at the offset at `0x3C` | `Machine` after it: `0x8664` x64, `0xAA64` arm64 | `Characteristics` 18 bytes on: `EXECUTABLE_IMAGE` set, `DLL` clear |
///
/// All three store their fields little-endian on these processors, and each header is read whole: 64 bytes for ELF, 32 for Mach-O, 24 from the PE signature.
///
/// A shared library is `ET_DYN` as a PIE is, so ELF tells the two apart only by the loader it names; the processor and the digest still hold for it.
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
    bool executable = false;                      ///< a program the loader starts, a library or an object file otherwise
    ProgramSystem system = ProgramSystem::Linux;  ///< the system whose loader reads it
    bool x64 = false;                             ///< built for x86-64
    bool arm64 = false;                           ///< built for arm64
};

inline Header elf(std::span<const uint8_t> b) {
    if (b.size() < 64 || b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F' || b[4] != 2) return {};
    return {true, le16(b, 16) == 2 || le16(b, 16) == 3, ProgramSystem::Linux, le16(b, 18) == 62, le16(b, 18) == 183};
}

inline Header machO(std::span<const uint8_t> b) {
    if (b.size() < 32 || le32(b, 0) != 0xFEEDFACFu) return {};
    return {true, le32(b, 12) == 2, ProgramSystem::MacOS, le32(b, 4) == 0x01000007u, le32(b, 4) == 0x0100000Cu};
}

inline Header pe(std::span<const uint8_t> b) {
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return {};
    const uint32_t peAt = le32(b, 0x3C);
    if (b.size() < static_cast<size_t>(peAt) + 24 || le32(b, peAt) != 0x00004550u) return {};
    const uint16_t traits = le16(b, peAt + 22);
    return {true, (traits & 0x0002) && !(traits & 0x2000), ProgramSystem::Windows, le16(b, peAt + 4) == 0x8664, le16(b, peAt + 4) == 0xAA64};
}
}  // namespace program_image

/// Why `head`, a program's first 4 KB, cannot replace the program of `system` on `cpu`, which the platform names, or nullptr when it can.
inline const char* programRejection(std::span<const uint8_t> head, ProgramSystem system, ProgramCpu cpu) {
    program_image::Header h = program_image::elf(head);
    if (!h.program) h = program_image::machO(head);
    if (!h.program) h = program_image::pe(head);
    if (!h.program) return "not a program";   // a 404 page, an archive, a board's image
    if (h.system != system) return "a program for another system";
    if (!h.executable) return "a library or an object file";
    return (cpu == ProgramCpu::Arm64 ? h.arm64 : h.x64) ? nullptr : "a program for another processor";
}

}  // namespace mm
