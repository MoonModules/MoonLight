/// Tempo harness: runs decoded audio through AudioService's block analysis and prints every frame.
///
/// The host half of moondeck/check/bench_tempo.py, which pipes in a file FFmpeg decodes; the analysis is the shipped one, through `AudioService::analyzeForTest`, so a figure the script reports describes the boards' code.
///
/// @technical
///
/// ## Input and output
///
/// Input is raw signed 32-bit little-endian mono at the service's rate on stdin, full scale being the 24-bit-left-justified regime the capture seam delivers.
///
/// Output is one tab-separated line per block, its time in milliseconds, then the frame's level, flux, onset and sixteen bands, after a `#` line naming the rate, the block and every column.
///
/// ## The clock
///
/// The analysis keeps its own clock, the samples read over the rate, so a two-minute track runs in about a second and the onsets read the same at any feed speed.
///
/// Each line's time is the block's first sample, one block before the analysis's clock, which counts to the block's end.
///
/// ## Why it is not a doctest
///
/// It reads files nobody commits and reports figures rather than asserting them, as the kernel micro-bench does.

#include "core/services/AudioService.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#if defined(_WIN32)
    #include <fcntl.h>   // _O_BINARY: stdin carries raw samples, which text mode would cut at 0x1A
    #include <io.h>
#endif

namespace {

/// The service's sample-rate select index for a rate in hertz, or -1 where the service offers none.
int rateIndex(uint32_t hz) {
    for (uint8_t i = 0; i < mm::AudioService::kSampleRateCount; i++)
        if (mm::AudioService::kSampleRates[i] == hz) return i;
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t rate = 24000;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            rate = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "usage: mm_tempo [--rate");
            for (uint8_t r = 0; r < mm::AudioService::kSampleRateCount; r++)
                std::fprintf(stderr, "%c%u", r ? '|' : ' ', static_cast<unsigned>(mm::AudioService::kSampleRates[r]));
            std::fprintf(stderr, "] < samples.s32le\n");
            return 2;
        }
    }
    const int sel = rateIndex(rate);
    if (sel < 0) {
        std::fprintf(stderr, "mm_tempo: %u Hz is not a rate AudioService offers\n", static_cast<unsigned>(rate));
        return 2;
    }

    // Defaults as a fresh card has them, at the rate asked, the analysis designed for it. On the heap, since its scratch is kilobytes.
    auto audio = std::make_unique<mm::AudioService>();
    audio->defineControls();
    audio->sampleRateSel = static_cast<uint8_t>(sel);
    audio->resetAnalysisForTest();

#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    constexpr size_t kBlock = mm::AudioService::kBlock;
    static int32_t block[kBlock];
    std::printf("# rate=%u block=%u columns=ms,level,flux,onset", static_cast<unsigned>(rate), static_cast<unsigned>(kBlock));
    for (uint8_t b = 0; b < 16; b++) std::printf(",b%u", static_cast<unsigned>(b));
    std::printf("\n");

    uint64_t samplesIn = 0;
    while (std::fread(block, sizeof(int32_t), kBlock, stdin) == kBlock) {
        const uint32_t nowMs = static_cast<uint32_t>(samplesIn * 1000u / rate);
        const mm::AudioFrame& f = audio->analyzeForTest(block);
        samplesIn += kBlock;
        std::printf("%u\t%u\t%u\t%u", static_cast<unsigned>(nowMs), static_cast<unsigned>(f.level),
                    static_cast<unsigned>(f.flux), static_cast<unsigned>(f.onset));
        for (uint8_t b = 0; b < 16; b++) std::printf("\t%u", static_cast<unsigned>(f.bands[b]));
        std::printf("\n");
    }
    return 0;
}
