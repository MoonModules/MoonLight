/// @module core

/// Pins what a desktop program's first bytes say about it, the check a desktop runs before a new version replaces it, as a board checks its image's chip.
///
/// An ELF, Mach-O or PE program for the right system and processor passes; another one, a library or an object file is refused with its reason, and a web page, an archive or a cut header is no program.

#include "doctest.h"
#include "core/util/ProgramImage.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace mm;

namespace {

void putLe16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = static_cast<uint8_t>(v); b[at + 1] = static_cast<uint8_t>(v >> 8); }
void putLe32(std::vector<uint8_t>& b, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) b[at + i] = static_cast<uint8_t>(v >> (8 * i)); }

std::vector<uint8_t> elf(uint16_t machine, uint16_t type = 3) {   // 3: a PIE, as the release builds
    std::vector<uint8_t> b(64, 0);
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2;
    putLe16(b, 16, type);
    putLe16(b, 18, machine);
    return b;
}

std::vector<uint8_t> machO(uint32_t cpu, uint32_t filetype = 2) {   // 2: MH_EXECUTE
    std::vector<uint8_t> b(32, 0);
    putLe32(b, 0, 0xFEEDFACFu);
    putLe32(b, 4, cpu);
    putLe32(b, 12, filetype);
    return b;
}

std::vector<uint8_t> pe(uint16_t machine, uint16_t characteristics = 0x0022) {   // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
    std::vector<uint8_t> b(0x100, 0);
    b[0] = 'M'; b[1] = 'Z';
    putLe32(b, 0x3C, 0x80);
    putLe32(b, 0x80, 0x00004550u);
    putLe16(b, 0x84, machine);
    putLe16(b, 0x96, characteristics);
    return b;
}

}  // namespace

TEST_CASE("a program for the right system and processor passes, in each format") {
    CHECK(programRejection(elf(62), ProgramSystem::Linux, ProgramCpu::X64) == nullptr);
    CHECK(programRejection(elf(183), ProgramSystem::Linux, ProgramCpu::Arm64) == nullptr);
    CHECK(programRejection(machO(0x0100000Cu), ProgramSystem::MacOS, ProgramCpu::Arm64) == nullptr);
    CHECK(programRejection(machO(0x01000007u), ProgramSystem::MacOS, ProgramCpu::X64) == nullptr);
    CHECK(programRejection(pe(0x8664), ProgramSystem::Windows, ProgramCpu::X64) == nullptr);
    CHECK(programRejection(pe(0xAA64), ProgramSystem::Windows, ProgramCpu::Arm64) == nullptr);
}

TEST_CASE("a program for another system or another processor is refused with that reason") {
    CHECK(std::string(programRejection(pe(0x8664), ProgramSystem::Linux, ProgramCpu::X64)) == "a program for another system");
    CHECK(std::string(programRejection(elf(62), ProgramSystem::MacOS, ProgramCpu::Arm64)) == "a program for another system");
    CHECK(std::string(programRejection(elf(62), ProgramSystem::Linux, ProgramCpu::Arm64)) == "a program for another processor");
    CHECK(std::string(programRejection(machO(0x01000007u), ProgramSystem::MacOS, ProgramCpu::Arm64)) == "a program for another processor");
}

TEST_CASE("a fixed-address ELF program passes as a PIE does, and a library or an object file is refused, in each format") {
    CHECK(programRejection(elf(62, 2), ProgramSystem::Linux, ProgramCpu::X64) == nullptr);   // ET_EXEC
    const char* const why = "a library or an object file";
    CHECK(std::string(programRejection(elf(62, 1), ProgramSystem::Linux, ProgramCpu::X64)) == why);               // ET_REL
    CHECK(std::string(programRejection(machO(0x0100000Cu, 6), ProgramSystem::MacOS, ProgramCpu::Arm64)) == why);  // MH_DYLIB
    CHECK(std::string(programRejection(pe(0x8664, 0x2022), ProgramSystem::Windows, ProgramCpu::X64)) == why);     // a DLL
}

TEST_CASE("a web page, an archive or a truncated header is no program") {
    const std::string page = "<html>404</html>";
    CHECK(std::string(programRejection(std::vector<uint8_t>(page.begin(), page.end()), ProgramSystem::Linux, ProgramCpu::X64)) == "not a program");
    CHECK(std::string(programRejection(std::vector<uint8_t>{0x1F, 0x8B, 0x08, 0x00}, ProgramSystem::MacOS, ProgramCpu::Arm64)) == "not a program");   // gzip
    std::vector<uint8_t> shortPe = pe(0x8664);
    shortPe.resize(0x82);   // the PE signature's offset points past the end
    CHECK(std::string(programRejection(shortPe, ProgramSystem::Windows, ProgramCpu::X64)) == "not a program");
    CHECK(std::string(programRejection({}, ProgramSystem::Windows, ProgramCpu::X64)) == "not a program");
    std::vector<uint8_t> shortElf = elf(62);
    shortElf.resize(20);   // magic, class and machine, the rest of the header missing
    CHECK(std::string(programRejection(shortElf, ProgramSystem::Linux, ProgramCpu::X64)) == "not a program");
}
