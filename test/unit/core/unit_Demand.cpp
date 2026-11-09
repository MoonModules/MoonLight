/// @module Demand
/// @also ModuleResource
/// @also Scheduler

/// Pins the demand a module places on another: it counts while its module is prepared and on, the provider hears every change, the last reader letting go ends it, and a demand on one provider never counts for another.

#include "doctest.h"
#include "core/module/Demand.h"
#include "core/module/Scheduler.h"
#include "core/services/AudioService.h"

using namespace mm;

namespace {

/// A provider that records what the tree demands of it each time it is told.
struct Provider : MoonModule {
    uint8_t heard = 0;   ///< what the tree demanded at the last telling
    int told = 0;        ///< how many times it was told
    Provider() { setTypeName("TestProvider"); setName("Provider"); }
    void onDemandChanged() override { told++; heard = Scheduler::instance()->demandsOf("TestProvider"); }
};

/// A reader that needs feature bit `bit` of `provider` while `wants` is on.
struct Reader : MoonModule {
    bool wants = true;
    Demand need;
    Reader(const char* name, const char* provider = "TestProvider", uint8_t bit = 2) : need(*this, provider, bit) { setTypeName("TestReader"); setName(name); }
    void prepare() override { need.set(wants); }
};

}  // namespace

TEST_CASE("a reader's demand reaches its provider while the reader is prepared, and leaves on its release") {
    Scheduler scheduler;
    auto* provider = new Provider();
    auto* reader = new Reader("Reader");
    scheduler.addModule(provider);
    scheduler.addModule(reader);
    scheduler.setup();
    reader->applyState();
    scheduler.preparePending();   // the frame boundary, where a provider hears of its demands
    CHECK(provider->heard == 2);
    reader->setEnabled(false);
    reader->applyState();   // disabled: released
    scheduler.preparePending();
    CHECK(provider->heard == 0);
    reader->setEnabled(true);
    reader->applyState();
    scheduler.preparePending();   // the frame boundary, where a provider hears of its demands
    CHECK(provider->heard == 2);
    scheduler.release();
}

TEST_CASE("a feature stays demanded until the last reader lets go, and a reader turning its need off lets go") {
    Scheduler scheduler;
    auto* provider = new Provider();
    auto* a = new Reader("A");
    auto* b = new Reader("B");
    scheduler.addModule(provider);
    scheduler.addModule(a);
    scheduler.addModule(b);
    scheduler.setup();
    a->applyState();
    b->applyState();
    a->release();
    scheduler.preparePending();
    CHECK(provider->heard == 2);   // B still reads it
    b->wants = false;
    b->applyState();
    scheduler.preparePending();
    CHECK(provider->heard == 0);
    const int told = provider->told;
    b->applyState();
    scheduler.preparePending();
    CHECK(provider->told == told);   // no change, no telling
    scheduler.release();
}

TEST_CASE("a demand on one provider never counts for another, whatever bit each numbers") {
    Scheduler scheduler;
    auto* provider = new Provider();
    auto* elsewhere = new Reader("Elsewhere", "OtherProvider", 2);
    scheduler.addModule(provider);
    scheduler.addModule(elsewhere);
    scheduler.setup();
    elsewhere->applyState();
    CHECK(scheduler.demandsOf("TestProvider") == 0);
    CHECK(scheduler.demandsOf("OtherProvider") == 2);
    scheduler.release();
}

// The notes are about half the analysis: AudioService runs them only while a reader needs them, or while it sends the MoonLight format, whose receivers show them.
TEST_CASE("AudioService wants the notes while a reader demands them or it sends the MoonLight format, and not otherwise") {
    Scheduler scheduler;
    auto* audio = new AudioService();
    audio->setName("Audio");
    struct NoteReader : MoonModule {
        AudioDemand notes{*this, AudioFeature::Notes};
        NoteReader() { setTypeName("TestNoteReader"); setName("Notes"); }
        void prepare() override { notes.set(true); }
    };
    auto* reader = new NoteReader();
    scheduler.addModule(audio);
    scheduler.addModule(reader);
    scheduler.setup();
    reader->applyState();
    CHECK(audio->notesWanted());
    reader->release();
    CHECK_FALSE(audio->notesWanted());
    audio->mode = AudioService::kLocalMode;
    audio->send = true;   // sending the MoonLight format, whose receivers show the notes
    CHECK(audio->notesWanted());
    audio->syncFormat = AudioService::kFormatWled;   // WLED's packet carries none
    CHECK_FALSE(audio->notesWanted());
    scheduler.release();
}

// The analysis is most of the audio's cost: AudioService runs it while a reader needs it, the notes need it, or it sends audio sync of either format, and rests otherwise.
TEST_CASE("AudioService wants the analysis while a reader or the notes need it or it sends audio sync, and not otherwise") {
    Scheduler scheduler;
    auto* audio = new AudioService();
    audio->setName("Audio");
    struct BandReader : MoonModule {
        AudioDemand bands{*this, AudioFeature::Bands};
        BandReader() { setTypeName("TestBandReader"); setName("Bands"); }
        void prepare() override { bands.set(true); }
    };
    auto* reader = new BandReader();
    scheduler.addModule(audio);
    scheduler.addModule(reader);
    scheduler.setup();
    reader->applyState();
    CHECK(audio->analysisWanted());
    reader->release();
    CHECK_FALSE(audio->analysisWanted());
    audio->setNotesForTest(true);   // the notes ride on the analysis
    CHECK(audio->analysisWanted());
    audio->setNotesForTest(false);
    audio->mode = AudioService::kLocalMode;
    audio->send = true;
    audio->syncFormat = AudioService::kFormatWled;   // even WLED's packet carries the bands
    CHECK(audio->analysisWanted());
    scheduler.release();
}

// The provider counts the tree again whenever it is told or prepares, so a demand placed before the provider exists still counts: the order modules arrive in never decides.
TEST_CASE("a demand placed before its provider exists counts once the provider arrives") {
    Scheduler scheduler;
    auto* reader = new Reader("Early");
    scheduler.addModule(reader);
    scheduler.setup();
    reader->applyState();   // told no one: there is no provider yet
    auto* provider = new Provider();
    scheduler.addModule(provider);
    CHECK(scheduler.demandsOf("TestProvider") == 2);   // what the provider counts when it prepares or is told
    reader->release();
    scheduler.preparePending();
    CHECK(provider->heard == 0);
    scheduler.release();
}

// Pre-commit review (2026-11-09): an effect replaced released its demand and the fresh one placed it again, so the provider switched off and on, freeing and resetting what it holds. Telling it at the frame boundary settles both at once.
TEST_CASE("a release and a prepare in one frame tell the provider once, with the demand still on") {
    Scheduler scheduler;
    auto* provider = new Provider();
    auto* old = new Reader("Old");
    scheduler.addModule(provider);
    scheduler.addModule(old);
    scheduler.setup();
    old->applyState();
    scheduler.preparePending();
    const int told = provider->told;
    auto* fresh = new Reader("Fresh");
    scheduler.addModule(fresh);
    old->release();     // the old one goes...
    fresh->applyState();   // ...and the new one comes, in the same frame
    CHECK(provider->told == told);   // not yet: nothing switches mid-frame
    scheduler.preparePending();
    CHECK(provider->told == told + 1);
    CHECK(provider->heard == 2);
    scheduler.release();
}

// Pre-commit review (2026-11-09): a reel runs its effect outside its children, so the walk missed the effect's demand and its notes stayed off.
TEST_CASE("a module a host runs outside its children counts through the host") {
    struct Host : MoonModule {
        Reader* current = nullptr;
        Host() { setTypeName("TestHost"); setName("Host"); }
        const MoonModule* hosted() const override { return current; }
    };
    Scheduler scheduler;
    auto* provider = new Provider();
    auto* host = new Host();
    scheduler.addModule(provider);
    scheduler.addModule(host);
    scheduler.setup();
    Reader hosted("Hosted");
    hosted.applyState();
    scheduler.preparePending();
    CHECK(provider->heard == 0);   // not reachable yet
    host->current = &hosted;
    hosted.release();
    hosted.applyState();
    scheduler.preparePending();
    CHECK(provider->heard == 2);
    host->current = nullptr;
    hosted.release();
    scheduler.release();
}
