/// @module MoonLive

/// Every shipped script compiles to the same front-end IR, which all five backends lower from, as it did before a compiler change.

#include "doctest.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/MoonLiveBuiltins_service.h"
#include "core/moonlive/MoonLiveScriptFile.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace mm;
using namespace mm::moonlive;

namespace {
// One line per shipped script; a script rewritten on purpose moves its hash, and the commit updating the line says why.
const std::map<std::string, uint64_t> kGolden = {
#include "moonlive_ir_golden.inc"
};

const BuiltinTable* g_table = nullptr;
const char* g_strings = nullptr;
uint64_t g_hash = 0;

// By name rather than table position, so a builtin added elsewhere in the table moves no script's hash.
uint64_t builtinName(HostCallFn fn) {
    if (!fn) return 0;
    for (uint8_t i = 0; i < g_table->count; i++) {
        if (g_table->items[i].fn != fn) continue;
        uint64_t h = 1469598103934665603ull;
        for (const char* c = g_table->items[i].name; *c; c++) { h ^= uint8_t(*c); h *= 1099511628211ull; }
        return h;
    }
    return 0xFFFF;   // a target outside the table, hashed as one value rather than as an address
}

uint64_t pointerOffset(const void* p) {
    if (!p) return 0;
    const char* c = static_cast<const char*>(p);
    if (g_strings && c >= g_strings && c < g_strings + CompileResult::kStringPool) return uint64_t(c - g_strings) + 1;
    return 0xFFFF;
}

// A lowering that hashes the IR instead of emitting code, and reports one byte so the compile counts as done.
size_t hashIr(IrProgram& ir, uint8_t*, size_t, const RegBudget*) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ull; }
    };
    mix(ir.count);
    mix(ir.localSlots);
    for (uint16_t i = 0; i < ir.count; i++) {
        const IrInst& in = ir.ops[i];
        mix(uint64_t(in.op));
        mix(in.dst); mix(in.a); mix(in.b); mix(in.c); mix(in.d);
        mix(uint32_t(in.imm));
        mix(builtinName(in.callFn));
        mix(pointerOffset(in.ptr));
        mix(uint64_t(in.inlineOp));
    }
    g_hash = h;
    return 1;
}

std::filesystem::path scriptRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "moonlive";
}

std::string read(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("every shipped script compiles to the IR it had before the compiler changed") {
    // MM_PRINT_IR_GOLDEN=1 prints the table instead of checking it, which is how it is captured.
    const bool print = std::getenv("MM_PRINT_IR_GOLDEN") != nullptr;
    int checked = 0;
    for (const char* sub : {"layouts", "effects", "modifiers", "palettes", "services"}) {
        const std::filesystem::path dir = scriptRoot() / sub;
        if (!std::filesystem::exists(dir)) continue;
        std::map<std::string, std::filesystem::path> files;   // sorted, so the printed table is stable
        for (const auto& e : std::filesystem::directory_iterator(dir))
            if (e.is_regular_file() && isScriptExt(e.path().extension().string().c_str())) files[e.path().filename().string()] = e.path();
        for (const auto& [name, path] : files) {
            const std::string label = std::string(sub) + "/" + name;
            const std::string src = read(path);
            const bool isService = std::string(sub) == "services";
            const SysVarTable sys = isService ? serviceSysVars()
                                  : std::string(sub) == "layouts" ? layoutSysVars()
                                  : std::string(sub) == "modifiers" ? modifierSysVars()
                                  : effectSysVars();
            const BuiltinTable table = isService ? serviceBuiltins() : lightBuiltins();
            static char strings[CompileResult::kStringPool];
            g_table = &table;
            g_strings = strings;
            g_hash = 0;
            uint8_t code[4];
            const CompileResult r = compileSource(src.c_str(), table, sys, code, sizeof(code), nullptr, hashIr, strings, sizeof(strings));
            CAPTURE(label);
            REQUIRE(r.ok);
            if (print) {
                std::printf("    {\"%s\", 0x%016llxull},\n", label.c_str(), static_cast<unsigned long long>(g_hash));
            } else {
                const auto it = kGolden.find(label);
                CHECK_MESSAGE(it != kGolden.end(), "no golden IR hash for " << label << ": capture it with MM_PRINT_IR_GOLDEN=1");
                if (it != kGolden.end()) CHECK(it->second == g_hash);
            }
            checked++;
        }
    }
    CHECK(checked > 0);
}
