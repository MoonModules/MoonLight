/// Tempo and note harness: runs decoded audio through AudioService's block analysis and prints every frame.
///
/// The host half of moondeck/check/bench_tempo.py and bench_notes.py, which pipe in a file FFmpeg decodes; the analysis is the shipped one, through `AudioService::analyzeForTest`, so a figure the scripts report describes the boards' code.
///
/// @technical
///
/// ## Input and output
///
/// Input is raw signed 32-bit little-endian mono at the service's rate on stdin, full scale being the 24-bit-left-justified regime the capture seam delivers.
///
/// Output is one tab-separated line per block, its time in milliseconds, then the frame's level, flux, onset and sixteen bands, after a `#` line naming the rate, the block and every column.
///
/// With `--notes` each line instead carries the keys each stage of the note effects keeps, through the effects' own `keep`: every sounding key as `key:level`, a `*` where it was struck; then the keys `no overtones` keeps, Keyboard's two hands and Strings' fretting hand, each with and without `chord`.
///
/// The last column is where that hand plays, `string:fret`; `-` stands for none. Keys count from A0, the frame's own numbering, and `--instrument` picks the Strings instrument.
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
#include "core/services/AudioChords.h"     // ChordTracker: the chord the effects' `chord` snaps to
#include "light/effects/KeyboardEffect.h"
#include "light/effects/StringsEffect.h"

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

/// The note effects' stages, each an effect of its own, since each keeps its own hands: @xref{input-and-output}.
struct NoteStages {
    mm::StringsEffect sieve, strings, stringsChord;
    mm::KeyboardEffect keys, keysChord;
    mm::ChordTracker chords;

    explicit NoteStages(uint8_t instrument) {
        sieve.notes = mm::AudioNotesBase::kNotesNoOvertones;
        for (mm::StringsEffect* e : {&strings, &stringsChord}) { e->notes = mm::AudioNotesBase::kNotesHands; e->instrument = instrument; }
        for (mm::KeyboardEffect* e : {&keys, &keysChord}) e->notes = mm::AudioNotesBase::kNotesHands;
        stringsChord.chord = keysChord.chord = true;
        strings.chord = keys.chord = false;
    }

    /// The keys `e` keeps, comma-separated, `-` for none.
    static void printKeys(const mm::AudioNotesBase& e) {
        bool any = false;
        for (uint8_t k = 0; k < mm::kAudioNoteKeys; k++)
            if (e.keepsForTest(k)) { std::printf("%s%u", any ? "," : "", static_cast<unsigned>(k)); any = true; }
        if (!any) std::printf("-");
    }

    /// One frame's line after its time: every stage's keys, then the fretting hand's strings and frets.
    void print(mm::AudioFrame f, uint16_t seq, uint32_t ms) {
        f.seq = seq;
        chords.name(f);
        for (mm::AudioNotesBase* e : std::initializer_list<mm::AudioNotesBase*>{&sieve, &keys, &keysChord, &strings, &stringsChord}) e->keep(f, ms);
        bool any = false;
        std::printf("\t");
        for (uint8_t k = 0; k < f.notesCount && k < mm::kAudioNoteKeys; k++)
            if (f.notes[k] > 0 || mm::hitAt(f.notesHits, k)) {
                std::printf("%s%u:%u%s", any ? "," : "", static_cast<unsigned>(k), static_cast<unsigned>(f.notes[k]), mm::hitAt(f.notesHits, k) ? "*" : "");
                any = true;
            }
        if (!any) std::printf("-");
        for (const mm::AudioNotesBase* e : std::initializer_list<const mm::AudioNotesBase*>{&sieve, &keys, &keysChord, &strings, &stringsChord}) { std::printf("\t"); printKeys(*e); }
        std::printf("\t");
        any = false;
        for (uint8_t s = 0; s < mm::hands::kMaxStrings; s++)
            if (strings.keyForTest(s) >= 0 && strings.keepsForTest(static_cast<uint8_t>(strings.keyForTest(s)))) { std::printf("%s%u:%d", any ? "," : "", static_cast<unsigned>(s), strings.fretForTest(s)); any = true; }
        if (!any) std::printf("-");
        std::printf("\n");
    }
};

}  // namespace

int main(int argc, char** argv) {
    uint32_t rate = 24000;
    bool notes = false;
    uint8_t instrument = mm::StringsEffect::kGuitar;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            rate = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--notes") == 0) {
            notes = true;
        } else if (std::strcmp(argv[i], "--instrument") == 0 && i + 1 < argc) {
            instrument = static_cast<uint8_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "usage: mm_tempo [--rate");
            for (uint8_t r = 0; r < mm::AudioService::kSampleRateCount; r++)
                std::fprintf(stderr, "%c%u", r ? '|' : ' ', static_cast<unsigned>(mm::AudioService::kSampleRates[r]));
            std::fprintf(stderr, "] [--notes [--instrument 0..3]] < samples.s32le\n");
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
    std::unique_ptr<NoteStages> stages;
    if (notes) {
        stages = std::make_unique<NoteStages>(instrument);
        std::printf("# rate=%u block=%u columns=ms,sounding,sieve,keys,keysChord,strings,stringsChord,place\n", static_cast<unsigned>(rate), static_cast<unsigned>(kBlock));
    } else {
        std::printf("# rate=%u block=%u columns=ms,level,flux,onset", static_cast<unsigned>(rate), static_cast<unsigned>(kBlock));
        for (uint8_t b = 0; b < 16; b++) std::printf(",b%u", static_cast<unsigned>(b));
        std::printf("\n");
    }

    uint64_t samplesIn = 0;
    while (std::fread(block, sizeof(int32_t), kBlock, stdin) == kBlock) {
        const uint32_t nowMs = static_cast<uint32_t>(samplesIn * 1000u / rate);
        const mm::AudioFrame& f = audio->analyzeForTest(block);
        samplesIn += kBlock;
        if (stages) {
            std::printf("%u", static_cast<unsigned>(nowMs));
            stages->print(f, static_cast<uint16_t>(samplesIn / kBlock), nowMs);
            continue;
        }
        std::printf("%u\t%u\t%u\t%u", static_cast<unsigned>(nowMs), static_cast<unsigned>(f.level),
                    static_cast<unsigned>(f.flux), static_cast<unsigned>(f.onset));
        for (uint8_t b = 0; b < 16; b++) std::printf("\t%u", static_cast<unsigned>(f.bands[b]));
        std::printf("\n");
    }
    return 0;
}
