/// Every shipped partition table has to be internally consistent. These are hand-edited CSVs whose offsets are absolute and whose mistakes are invisible until a board fails to boot or an OTA silently truncates, so the arithmetic is pinned here rather than discovered on hardware.
/// Rules come from the ESP-IDF partition-table format: partitions may not overlap, must sit inside the flash the table is written for, must not start before the table itself ends, and an APP partition must be 64 KB aligned (the MMU maps app code in 64 KB pages).

#include "doctest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Partition {
    std::string name, type, subtype;
    uint32_t offset = 0, size = 0;
    uint32_t end() const { return offset + size; }
};

struct Table {
    std::string file;
    uint32_t flashBytes = 0;   // the flash the table is written for; declaredFlashBytes below
    std::vector<Partition> parts;
};

// Each table's flash capacity, DECLARED rather than inferred from the largest end offset: an inferred capacity would grow with an oversized table and hide exactly the overflow this suite exists to catch. A new table must be added here, which is the point.
uint32_t declaredFlashBytes(const std::string& file) {
    static const std::pair<const char*, uint32_t> kCapacity[] = {
        {"esp32dev.csv",          4u * 1024 * 1024},
        {"4mb_moonbase.csv",      4u * 1024 * 1024},
        {"wled_4mb_1mb_fs.csv",   4u * 1024 * 1024},
        {"esp32dev_8mb_moonbase.csv", 8u * 1024 * 1024},
        {"esp32s3_n8r8.csv",      8u * 1024 * 1024},
        {"ota_16mb.csv",         16u * 1024 * 1024},
        {"ota_16mb_moonbase.csv", 16u * 1024 * 1024},
    };
    for (const auto& [name, bytes] : kCapacity)
        if (file == name) return bytes;
    FAIL("partition table ", file, " has no declared flash capacity; add it to kCapacity");
    return 0;
}

std::filesystem::path partitionDir() {
    // test/unit/core/<this file> -> repo root -> esp32/partitions
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path()
           / "esp32" / "partitions";
}

// The CSV allows sizes and offsets as hex (0x...) or as a decimal count with a K/M suffix.
bool parseNumber(std::string tok, uint32_t& out) {
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
    if (tok.empty()) return false;
    char suffix = tok.back();
    uint32_t mult = 1;
    if (suffix == 'K' || suffix == 'k') { mult = 1024; tok.pop_back(); }
    else if (suffix == 'M' || suffix == 'm') { mult = 1024 * 1024; tok.pop_back(); }
    if (tok.empty()) return false;
    // strtoul silently wraps a negative token; a partition offset or size is never signed.
    if (tok.front() == '-' || tok.front() == '+') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long v = std::strtoul(tok.c_str(), &end, 0);   // base 0: 0x.. is hex
    if (errno != 0 || end == tok.c_str() || *end != '\0') return false;
    // The K/M multiply (and the plain value) must fit uint32: 4096M or 0x100000000 is a typo, not a 4 GB partition.
    if (v > UINT32_MAX / mult) return false;
    out = static_cast<uint32_t>(v) * mult;
    return true;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

Table readTable(const std::filesystem::path& path) {
    Table t;
    t.file = path.filename().string();
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "cannot open ", path.string());
    std::string line;
    while (std::getline(in, line)) {
        const std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        std::vector<std::string> f;
        size_t start = 0;
        while (true) {
            const size_t comma = s.find(',', start);
            f.push_back(trim(s.substr(start, comma == std::string::npos ? comma : comma - start)));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (f.size() < 5) continue;
        Partition p;
        p.name = f[0]; p.type = f[1]; p.subtype = f[2];
        REQUIRE_MESSAGE(parseNumber(f[3], p.offset), t.file, ": bad offset for ", p.name);
        REQUIRE_MESSAGE(parseNumber(f[4], p.size), t.file, ": bad size for ", p.name);
        t.parts.push_back(p);
    }
    t.flashBytes = declaredFlashBytes(t.file);
    return t;
}

std::vector<Table> allTables() {
    std::vector<Table> out;
    for (const auto& e : std::filesystem::directory_iterator(partitionDir()))
        if (e.path().extension() == ".csv") out.push_back(readTable(e.path()));
    return out;
}

}  // namespace

TEST_CASE("the CSV number parser rejects signed and uint32-overflowing values") {
    uint32_t v = 0;
    CHECK_FALSE(parseNumber("-1", v));            // strtoul would wrap this to 4 GB - 1
    CHECK_FALSE(parseNumber("4096M", v));         // 4 GiB: past uint32 after the suffix multiply
    CHECK_FALSE(parseNumber("0x100000000", v));   // past uint32 as a plain value
    CHECK(parseNumber("0x3E9000", v));
    CHECK(v == 0x3E9000u);
    CHECK(parseNumber("512K", v));
    CHECK(v == 512u * 1024u);
}

TEST_CASE("every partition table describes a layout that fits its flash without overlaps") {
    const auto tables = allTables();
    REQUIRE(tables.size() >= 3);   // esp32dev, esp32s3_n8r8, ota_16mb at time of writing

    for (const auto& t : tables) {
        CAPTURE(t.file);
        REQUIRE(t.parts.size() >= 3);

        // The bootloader lives below 0x8000 and the partition table itself at 0x8000, so no partition may start before 0x9000 (the first usable offset ESP-IDF documents).
        for (const auto& p : t.parts) {
            CAPTURE(p.name);
            CHECK(p.offset >= 0x9000u);
            CHECK(p.size > 0u);
            CHECK(p.end() <= t.flashBytes);
        }

        // No two partitions may overlap. Compared pairwise rather than by sorting, so the failure message names both culprits.
        for (size_t i = 0; i < t.parts.size(); i++) {
            for (size_t j = i + 1; j < t.parts.size(); j++) {
                const auto& a = t.parts[i];
                const auto& b = t.parts[j];
                const bool disjoint = a.end() <= b.offset || b.end() <= a.offset;
                CAPTURE(a.name);
                CAPTURE(b.name);
                CHECK(disjoint);
            }
        }
    }
}

TEST_CASE("app partitions are 64 KB aligned, as the MMU requires") {
    for (const auto& t : allTables()) {
        CAPTURE(t.file);
        for (const auto& p : t.parts) {
            if (p.type != "app") continue;
            CAPTURE(p.name);
            CHECK((p.offset % 0x10000u) == 0u);
        }
    }
}

TEST_CASE("a table carries either two OTA slots or one slot plus a recovery app, never a mix") {
    // Dual-OTA (ota_0 + ota_1) buys a power-fail rollback at the cost of holding two copies of the firmware. The safeboot shape (factory + ota_0) spends that space on the app instead and recovers through the factory image. Both are valid; a table that has ota_1 AND a factory partition would be paying for both and is a mistake.
    for (const auto& t : allTables()) {
        CAPTURE(t.file);
        int ota = 0, factory = 0;
        for (const auto& p : t.parts) {
            if (p.type != "app") continue;
            if (p.subtype == "factory") factory++;
            else if (p.subtype.starts_with("ota_")) ota++;
        }
        CHECK(factory <= 1);
        CHECK(ota >= 1);
        const bool dualOta = (ota == 2 && factory == 0);
        const bool safeboot = (ota == 1 && factory == 1);
        CHECK((dualOta || safeboot));
    }
}

TEST_CASE("every MoonBase factory slot can be erased and holds a MoonBase image") {
    // The app installs a new MoonBase by erasing this partition and streaming into it, and esp_partition_erase_range works in whole 4 KB sectors: an offset or size that is not a multiple would erase past the slot or leave a tail behind. Both are 64 KB aligned today (the app-alignment rule above), which satisfies this, but the erase depends on the weaker property and should say so rather than inherit it by luck.
    constexpr uint32_t kSector = 4096;
    constexpr uint32_t kImageBytes = 743 * 1024;   // the built image, which the slot must hold
    int factories = 0;
    for (const auto& t : allTables()) {
        CAPTURE(t.file);
        for (const auto& p : t.parts) {
            if (p.type != "app" || p.subtype != "factory") continue;
            ++factories;
            CAPTURE(p.name);
            CHECK((p.offset % kSector) == 0u);
            CHECK((p.size % kSector) == 0u);
            CHECK(p.size >= kImageBytes);
        }
    }
    // A zero here would mean the tables stopped carrying MoonBase, or that this test stopped finding them: either way the checks above proved nothing.
    CHECK(factories > 0);
}
