/// Kernel micro-bench: nanoseconds per call for the power-function kernels, on the host.
///
/// The gate for a kernel swap.
/// The generative-fields plan replaces value noise with gradient noise behind the same names and accepts the swap only within 1.3x of the value-noise cost per sample.
/// That bound is meaningless without the number it is measured against, so this target records it, before the swap, and every later kernel adds a row.
/// Host timings, not ESP32 cycles: the S3 is 20-40x slower per core (performance.md, the `collide` measurement) and the ratio between two rows is what transfers, not the absolute figure.
///
/// @technical
///
/// ## Method
///
/// Sweep a 256x256 grid of 16.0 fixed coordinates at a fixed step (the same shape a noise effect samples), accumulate every result into a checksum the compiler cannot elide, repeat, and keep the best of several runs so a scheduler hiccup does not become a slower kernel.
/// Output is a Markdown table so the rows paste into performance.md unchanged.
///
/// ## Why it is not a doctest
///
/// A benchmark that asserts a timing is a flaky test, and one that does not assert is a report.
/// This is the report.
/// Run by hand or through moondeck/check/bench_kernels.py.

#include "core/util/math16.h"
#include "core/util/noise.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint32_t kGrid = 256;          // samples per axis per pass
constexpr uint32_t kStep = 40;           // 16.0 fixed coordinate step: ~6.4 noise cells across
constexpr int kRuns = 5;                 // best-of

/// Time one kernel over the grid; returns the best ns per sample across kRuns.
///
/// The kernel arrives as a TEMPLATE parameter, not a std::function.
/// A type-erased call cannot be inlined, so it adds an indirect call to every sample and lands in the same nanoseconds the measurement is trying to attribute to the kernel.
/// At 5 ns a sample that is a large share of what is being reported.
template <typename Fn>
double bench(Fn fn) {
    double best = 1e18;
    volatile uint32_t sink = 0;                 // the checksum the optimizer must honor
    for (int run = 0; run < kRuns; run++) {
        uint32_t acc = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t y = 0; y < kGrid; y++)
            for (uint32_t x = 0; x < kGrid; x++)
                acc += fn(x * kStep, y * kStep, (x + y) * 3);
        const auto t1 = std::chrono::steady_clock::now();
        sink = sink + acc;
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (kGrid * kGrid);
        if (ns < best) best = ns;
    }
    return best;
}

/// One row of the table: measure `fn` and print it.
template <typename Fn>
void row(const char* name, Fn fn) {
    const double ns = bench(fn);
    std::printf("| %s | %.1f | %.1f |\n", name, ns, 1000.0 / ns);
}

}  // namespace

int main() {
    using namespace mm;
    std::printf("| Kernel | ns/sample | Msamples/s |\n|---|---:|---:|\n");
    row("inoise8 1D",          [](uint32_t x, uint32_t, uint32_t)     { return uint32_t(inoise8(x)); });
    row("inoise8 2D",          [](uint32_t x, uint32_t y, uint32_t)   { return uint32_t(inoise8(x, y)); });
    row("inoise8 3D",          [](uint32_t x, uint32_t y, uint32_t z) { return uint32_t(inoise8(x, y, z)); });
    row("inoise16 1D",         [](uint32_t x, uint32_t, uint32_t)     { return uint32_t(inoise16(x)); });
    row("inoise16 2D",         [](uint32_t x, uint32_t y, uint32_t)   { return uint32_t(inoise16(x, y)); });
    row("inoise16 3D",         [](uint32_t x, uint32_t y, uint32_t z) { return uint32_t(inoise16(x, y, z)); });
    row("fbm8 2D, 2 octaves",  [](uint32_t x, uint32_t y, uint32_t)   { return uint32_t(fbm8(x, y, 2)); });
    row("fbm8 2D, 4 octaves",  [](uint32_t x, uint32_t y, uint32_t)   { return uint32_t(fbm8(x, y, 4)); });
    row("fbm16 2D, 2 octaves", [](uint32_t x, uint32_t y, uint32_t)   { return uint32_t(fbm16(x, y, 2)); });
    row("fbm16 2D, 4 octaves", [](uint32_t x, uint32_t y, uint32_t)   { return uint32_t(fbm16(x, y, 4)); });
    row("turbulence8 2D, 2 octaves", [](uint32_t x, uint32_t y, uint32_t) { return uint32_t(turbulence8(x, y, 2)); });
    row("warp8 2D, 1 octave",  [](uint32_t x, uint32_t y, uint32_t)   { return uint32_t(warp8(x, y, 512, 1)); });
    row("warp8 2D, 2 octaves", [](uint32_t x, uint32_t y, uint32_t)   { return uint32_t(warp8(x, y, 512, 2)); });
    row("atan16",              [](uint32_t x, uint32_t y, uint32_t)   { return uint32_t(atan16(int32_t(y) - 128, int32_t(x) - 128)); });
    row("dist16",              [](uint32_t x, uint32_t y, uint32_t)   { return dist16(int32_t(x) - 128, int32_t(y) - 128); });
    row("polar address (dist16 + atan16 + kaleido)", [](uint32_t x, uint32_t y, uint32_t) {
        const int32_t dx = int32_t(x) - 128, dy = int32_t(y) - 128;
        const uint32_t r = dist16(dx, dy);
        return uint32_t(kaleido(angle16(atan16(dy, dx) + r * 4), 5)) + r;
    });
    return 0;
}
