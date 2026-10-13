/// @module FirmwareUpdate
///
/// The checks an upload passes before it overwrites a Teensy's only program, each refusal named, and the real built image accepted when the build tree has one.

#include "doctest.h"
#include "core/util/TeensyImage.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr char kMark[] = "MoonLight image for teensy41;";
constexpr uint32_t kPrefixLen = sizeof("MoonLight image for ") - 1;

// A minimal Teensy 4 image: the FlexSPI config tag, the IVT's first byte, and a mark naming `variant`.
std::vector<uint8_t> image(const char* variant) {
    std::vector<uint8_t> img(0x2000, 0xFF);
    std::memcpy(img.data(), "FCFB", 4);
    img[mm::firmware::kTeensyIvtOffset] = 0xD1;
    if (variant) {
        const std::string mark = std::string("MoonLight image for ") + variant + ";";
        std::memcpy(img.data() + 0x1800, mark.data(), mark.size());
    }
    return img;
}

std::string refusal(const std::vector<uint8_t>& img) {
    char status[96] = {};
    if (!mm::firmware::teensyImageRefused(img.data(), static_cast<uint32_t>(img.size()), kMark, kPrefixLen, "teensy41", status, sizeof(status))) return "";
    return status;
}

}  // namespace

TEST_CASE("a HEX file or anything but a Teensy 4 image is refused from its first bytes") {
    const uint8_t hex[] = {':', '1', '0', '0'};
    CHECK(std::string(mm::firmware::teensyHeaderProblem(hex, sizeof(hex))) == "error: that is a HEX file, upload the .bin");
    const uint8_t esp[] = {0xE9, 0x05, 0x02, 0x20};
    CHECK(std::string(mm::firmware::teensyHeaderProblem(esp, sizeof(esp))) == "error: not a Teensy 4 image");
    CHECK(mm::firmware::teensyHeaderProblem(image("teensy41").data(), 4) == nullptr);
}

TEST_CASE("a Teensy image without its vector table, without a MoonLight mark, or for another firmware is refused") {
    std::vector<uint8_t> noIvt = image("teensy41");
    noIvt[mm::firmware::kTeensyIvtOffset] = 0xFF;
    CHECK(refusal(noIvt) == "error: not a Teensy 4 image");
    CHECK(refusal(image(nullptr)) == "error: that image is not MoonLight");
    CHECK(refusal(image("teensy40")) == "error: image is teensy40, board runs teensy41");
    CHECK(refusal(image("teensy41")).empty());
}

TEST_CASE("the built Teensy image passes every check, when the build tree has one") {
    const std::filesystem::path bin = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "build/teensy41/MoonLight.bin";
    if (!std::filesystem::exists(bin)) return;   // a tree that never built the Teensy, as on the desktop CI jobs
    std::ifstream f(bin, std::ios::binary);
    const std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(img.size() > mm::firmware::kTeensyIvtOffset);
    CHECK(mm::firmware::teensyHeaderProblem(img.data(), img.size()) == nullptr);
    CHECK(refusal(img).empty());
}
