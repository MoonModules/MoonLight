/// The test binary's main, and what every test case shares: the suite-wide fixtures live here, not in any one module's tests.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "platform/platform.h"   // setTestNowMs, setTestClockStep: the shared test clock

// Every test case, and every rerun for its next subcase, starts on the real clock, so a test that froze or stepped it leaves nothing to the next.
struct RealClockPerTest : doctest::IReporter {
    explicit RealClockPerTest(const doctest::ContextOptions&) {}
    static void reset() { mm::platform::setTestNowMs(0); mm::platform::setTestClockStep(0); }
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
REGISTER_LISTENER("real clock per test", 1, RealClockPerTest);
