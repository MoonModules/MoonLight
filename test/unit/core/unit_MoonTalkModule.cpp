/// @module MoonTalkModule

#include "doctest.h"
#include "core/system/MoonTalkModule.h"
#include "core/module/Scheduler.h"
#include "core/system/SystemModule.h"

#include <string>

/// Naming is a SECOND consent on top of posting, and it starts off.
///
/// A device name identifies a person where an installation id does not, so agreeing to publish messages is not agreeing to be named.
/// `sharesName()` requires BOTH, which is what keeps the two decisions separate.
TEST_CASE("naming is a second consent, off by default") {
    mm::MoonTalkModule talk;
    talk.defineControls();

    CHECK_FALSE(talk.consent());
    CHECK_FALSE(talk.sharesName());

    // The whole matrix, because `sharesName()` is an AND and either half alone must not publish a name: consenting to post is not consenting to be named, and vice versa.
    talk.setShareNameForTest(true);
    CHECK_FALSE(talk.sharesName());                     // named, but not allowed to post

    talk.setShareNameForTest(false);
    talk.setConsentForTest(true);
    CHECK_FALSE(talk.sharesName());                     // allowed to post, but anonymously

    talk.setShareNameForTest(true);
    CHECK(talk.sharesName());                           // both, and only then
}

/// The device name is READ from the module tree, never held as a copy.
///
/// An earlier shape had a `setDeviceName()` setter that nothing ever called, so the name stayed empty and every message went out anonymous while the `shareName` toggle said otherwise.
/// The UI promised something the wire did not deliver, and only reading the stored rows revealed it.
///
/// With no System module present the lookup yields an empty string rather than misbehaving, which is the case a probe instance (built by /api/types and thrown away) actually hits.
TEST_CASE("the device name is looked up rather than stored") {
    mm::MoonTalkModule talk;
    talk.defineControls();

    // With no System module in the tree: empty, not a crash. This is the case a probe instance (built by /api/types and thrown away) actually hits.
    const char* name = talk.deviceName();
    REQUIRE(name != nullptr);            // never null, whatever the tree holds
    CHECK(std::string(name).empty());
}

/// And with a System module present it returns THAT name, which is the half the previous case could not see: a lookup that always returned "" would have passed it.
TEST_CASE("the device name comes from the System module") {
    // Heap-allocated and owned by the scheduler, which deletes its tree on release(): a stack module handed to addModule() is deleted as stack memory, which segfaults.
    mm::Scheduler scheduler;
    auto* system = new mm::SystemModule();
    system->setTypeName("SystemModule");
    system->setName("System");
    system->setScheduler(&scheduler);
    scheduler.addModule(system);
    // setup() publishes `instance_` (which deviceName() walks) AND runs each module's setup(), the MAC fallback that fills deviceName_. Without it the lookup finds no tree and returns "".
    scheduler.setup();

    const std::string expected = system->deviceName();
    REQUIRE_FALSE(expected.empty());   // or the comparison below proves nothing

    mm::MoonTalkModule talk;
    talk.defineControls();

    const char* fromTree = talk.deviceName();
    REQUIRE(fromTree != nullptr);
    CHECK(std::string(fromTree) == expected);

    scheduler.release();
}

/// Pressing `send` is the only thing that publishes, and it empties the box.
///
/// A Text control reports every debounced KEYSTROKE, so an earlier shape published "hel" and "hell" while someone typed "hello".
/// A settle window then guessed when typing had stopped and cleared half-typed messages when it guessed wrong.
/// The button removes the guess: typing changes nothing, and the message survives until the user says so.
TEST_CASE("a message is published by the send button, not by typing") {
    mm::MoonTalkModule talk;
    talk.defineControls();
    talk.setConsentForTest(true);
    talk.setMessageForTest("hello");

    // Typing does NOT publish and does NOT clear: the box still holds what was typed.
    talk.onControlChanged("message");
    CHECK(std::string(talk.message()) == "hello");

    // Nor does any other control.
    talk.onControlChanged("shareName");
    CHECK(std::string(talk.message()) == "hello");

    // Pressing send with no MoonCloud parent publishes nothing, and the text SURVIVES. Clearing on a failed hand-off threw away what somebody typed at the moment they would most want to retry.
    talk.onControlChanged("send");
    CHECK(std::string(talk.message()) == "hello");
}

/// Consent still gates publishing, and a refused send leaves the text alone rather than discarding it: withholding consent is not a reason to lose what somebody wrote.
TEST_CASE("send without consent publishes nothing and keeps the text") {
    mm::MoonTalkModule talk;
    talk.defineControls();
    talk.setMessageForTest("hello");

    REQUIRE_FALSE(talk.consent());
    talk.onControlChanged("send");
    CHECK(std::string(talk.message()) == "hello");

    talk.setConsentForTest(false);
    talk.onControlChanged("send");
    CHECK(std::string(talk.message()) == "hello");
}

/// An empty box sends nothing, so a stray press cannot publish a blank message.
TEST_CASE("send with an empty message does nothing") {
    mm::MoonTalkModule talk;
    talk.defineControls();
    talk.setConsentForTest(true);

    talk.onControlChanged("send");
    CHECK(std::string(talk.message()).empty());
}


/// Talk carries its own explanation, because what it exchanges is nothing like a report.
TEST_CASE("the talk consent explanation is a status that follows the answer") {
    mm::MoonTalkModule talk;
    talk.setup();
    talk.defineControls();

    REQUIRE_FALSE(talk.consent());
    REQUIRE(talk.status() != nullptr);
    CHECK(std::string(talk.status()).find("public board") != std::string::npos);

    talk.setConsentForTest(true);
    CHECK(talk.status() == nullptr);
}

namespace {
int g_talkStatus = 200;
int talkStandIn(const char*, const char*, const char*, char*, size_t) { return g_talkStatus; }
}  // namespace

/// The box keeps the message until the server took it, so an unreachable server never costs what somebody typed.
TEST_CASE("a message leaves the box only once it arrived") {
    if (!mm::platform::httpsAvailable()) { MESSAGE("no HTTPS client in this build"); return; }
    mm::platform::setTestHttpFetch(&talkStandIn);
    for (const int status : {0, 200}) {
        g_talkStatus = status;
        mm::Scheduler scheduler;
        auto* cloud = new mm::MoonCloudModule(mm::MoonCloudModule::Route::Direct);
        cloud->setName("MoonCloud");
        auto* talk = new mm::MoonTalkModule();
        talk->setName("Talk");
        cloud->addChild(talk);
        scheduler.addModule(cloud);
        scheduler.setup();
        talk->setConsentForTest(true);
        talk->setMessageForTest("hello");
        talk->onControlChanged("send");
        CHECK(std::string(talk->message()) == "hello");   // on its way: still in the box
        REQUIRE(mm::JsonFetch::instance().settle(2000));
        talk->tick1s();
        CHECK(std::string(talk->message()) == (status == 200 ? "" : "hello"));
        scheduler.release();
    }
    mm::platform::setTestHttpFetch(nullptr);
}
