/// The test binary's main, and what every test case shares: the suite-wide fixtures live here, not in any one module's tests.

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <cstdint>
#include <cstdio>

#include "core/util/ClockSync.h"   // FleetTime: the fleet clock, back to the local clock per case
#include "light/util/Palette.h"    // Palettes: each case starts on the default palette
#include "platform/Hub75Pio.h"    // setTestHub75PioGateOpen: no case opens the real PIO; hub75PioTestRealOpens: the count the run ends on
#include "platform/machine.h"     // setTestMachineRest: no case depends on the board it runs on
#include "platform/platform.h"   // setTestNowMs, setTestClockStep: the shared test clock; hdmiTestClear: the HDMI stand-in; setTestJoin: multicast off the host's network

// Every case and subcase rerun starts on the real clock and its own local time, the HDMI stand-in, a multicast off the host's network, an `Other` machine, the real PIO out of reach and the default palette.
struct FreshFixturesPerTest : doctest::IReporter {
    explicit FreshFixturesPerTest(const doctest::ContextOptions&) {}
    static void reset() {
        mm::platform::setTestNowMs(0);
        mm::platform::setTestClockStep(0);
        mm::platform::hdmiTestClear();
        mm::platform::setTestJoin(mm::platform::TestJoin::Succeed);
        mm::FleetTime::reset();   // the process-wide fleet time, which a case's Devices tree may have claimed
        mm::platform::setTestMachineRest(mm::platform::Machine::Other);
        mm::platform::setTestHub75PioGateOpen(false);
        mm::Palettes::applyRequest();   // a request a case's DemoReel left pending, taken here rather than inside the next case's first frame
        mm::Palettes::setActive(0);     // and the default palette back
    }
    void test_case_start(const doctest::TestCaseData&) override { reset(); }
    void test_case_reenter(const doctest::TestCaseData&) override { reset(); }
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};
REGISTER_LISTENER("fresh fixtures per test", 1, FreshFixturesPerTest);

// The suite's main: safe defaults, the run, then the proof that no case opened the real PIO. The check follows run(), so a JUnit report is always written whole, and the exit status carries the failure.
int main(int argc, char** argv) {
    FreshFixturesPerTest::reset();
    doctest::Context context(argc, argv);
    int result = context.run();
    if (context.shouldExit()) return result;   // a query such as --list-test-cases
    if (const uint32_t opens = mm::platform::hub75PioTestRealOpens()) {
        // A case that made the real backend got past the closed gate (hub75_pio_backend.h, Real-backend gate).
        std::fprintf(stderr, "FAIL: %u case(s) opened the real PIO backend (/dev/pio0)\n", static_cast<unsigned>(opens));
        result = 1;
    }
    return result;
}
