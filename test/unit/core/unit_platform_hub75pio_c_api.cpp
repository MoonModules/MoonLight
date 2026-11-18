/// @module platform

/// Pins the C shim between the piolib backend and piolib (hub75_pio_c_api.c), one level below the backend's own test: the shim's source is compiled here, as C++, against a scripted stand-in for piolib.h (piolib_fake/piolib.h) whose functions this file defines.
///
/// It shows the cached handle, the same-state-machine preference and its fallback, when the DMA state machine is remembered, and how each failure maps to an errno.
///
/// The shim's statics live for the process, so the cases run in the order written, each leaving the state the next expects. What this cannot show is piolib itself: the real kernel behavior behind these calls is the Pi's to prove (the bench, U7). That the shim is valid C is checked by compiling it as C against the real header.

#include "doctest.h"

#ifndef MM_HUB75_PIO_HAVE_PIOLIB

#include "piolib.h"   // the stand-in, from piolib_fake/
#include "platform/rpi/hub75_pio_c_api.h"

#include <cerrno>
#include <string>
#include <vector>

namespace {

// What the scripted piolib does, and what it was asked.
struct Piolib {
    int openErr = 0;                // pio_open fails with this positive errno
    int opens = 0;                  // pio_open calls
    int unusedSm = 1;               // what pio_claim_unused_sm answers: an SM, or a negative errno
    unsigned claimedMask = 0;       // the state machines another client holds
    bool claimFails = false;        // pio_sm_claim sets the error flag
    int xferConfigRc = 0;
    bool errorFlag = false;         // pio_get_error
    bool addFlagsError = false;     // pio_add_program sets the error flag
    uint addOffset = 3;             // what pio_add_program answers
    int initPc = -1;                // the program counter pio_sm_init was given
    std::vector<std::string> calls;
};
Piolib lib;

std::vector<std::string> callsOf(const char* name) {
    std::vector<std::string> out;
    for (const std::string& c : lib.calls)
        if (c.rfind(name, 0) == 0) out.push_back(c);
    return out;
}

}  // namespace

extern "C" {
PIO pio_open(uint) {
    lib.opens++;
    lib.calls.push_back("pio_open");
    return lib.openErr ? PIO_ERR(-lib.openErr) : reinterpret_cast<PIO>(&lib);
}
void pio_select(PIO) { lib.calls.push_back("pio_select"); }
void pio_enable_fatal_errors(PIO, bool enable) { lib.calls.push_back(enable ? "fatal_on" : "fatal_off"); }
void pio_clear_error(PIO) { lib.errorFlag = false; }
bool pio_get_error(PIO) { return lib.errorFlag; }
int pio_claim_unused_sm(PIO, bool) {
    lib.calls.push_back("claim_unused");
    return lib.unusedSm;
}
bool pio_sm_is_claimed(PIO, uint sm) { return (lib.claimedMask >> sm) & 1u; }
void pio_sm_claim(PIO, uint sm) {
    lib.calls.push_back("claim " + std::to_string(sm));
    if (lib.claimFails) lib.errorFlag = true;
}
void pio_sm_unclaim(PIO, uint sm) { lib.calls.push_back("unclaim " + std::to_string(sm)); }
uint pio_add_program(PIO, const pio_program_t*) {
    if (lib.addFlagsError) lib.errorFlag = true;
    return lib.addOffset;
}
void pio_remove_program(PIO, const pio_program_t*, uint) {}
int pio_sm_config_xfer(PIO, uint sm, uint, uint, uint) {
    lib.calls.push_back("config_xfer " + std::to_string(sm));
    return lib.xferConfigRc;
}
int pio_sm_xfer_data(PIO, uint, uint, uint, void*) { return 0; }
void pio_sm_set_pins_with_mask(PIO, uint, uint32_t, uint32_t) {}
void pio_sm_set_consecutive_pindirs(PIO, uint, uint, uint, bool) {}
void pio_gpio_init(PIO, uint) {}
pio_sm_config pio_get_default_sm_config(void) { return pio_sm_config{}; }
void sm_config_set_wrap(pio_sm_config*, uint, uint) {}
void sm_config_set_sideset(pio_sm_config*, uint, bool, bool) {}
void sm_config_set_out_pins(pio_sm_config*, uint, uint) {}
void sm_config_set_sideset_pins(pio_sm_config*, uint) {}
void sm_config_set_out_shift(pio_sm_config*, bool, bool, uint) {}
void sm_config_set_fifo_join(pio_sm_config*, uint) {}
void sm_config_set_clkdiv_int_frac(pio_sm_config*, uint16_t, uint8_t) {}
void pio_sm_init(PIO, uint, uint initial_pc, const pio_sm_config*) {
    lib.calls.push_back("pio_sm_init");
    lib.initPc = static_cast<int>(initial_pc);
}
void pio_sm_clear_fifos(PIO, uint) {}
void pio_sm_set_enabled(PIO, uint, bool) {}
bool pio_sm_is_tx_fifo_empty(PIO, uint) { return true; }
}

// The shim's own source, compiled against the scripted piolib above. The backend's test defines scripted shim functions under the real names in this same executable, so the shim's are renamed here.
#define hub75_pio_open shim_hub75_pio_open
#define hub75_pio_claim_sm shim_hub75_pio_claim_sm
#define hub75_pio_add_program shim_hub75_pio_add_program
#define hub75_pio_config_xfer shim_hub75_pio_config_xfer
#define hub75_pio_init_sm shim_hub75_pio_init_sm
#define hub75_pio_clear_fifos shim_hub75_pio_clear_fifos
#define hub75_pio_set_enabled shim_hub75_pio_set_enabled
#define hub75_pio_xfer shim_hub75_pio_xfer
#define hub75_pio_tx_fifo_empty shim_hub75_pio_tx_fifo_empty
#define hub75_pio_remove_program shim_hub75_pio_remove_program
#define hub75_pio_release shim_hub75_pio_release
#include "platform/rpi/hub75_pio_c_api.c"
#undef hub75_pio_open
#undef hub75_pio_claim_sm
#undef hub75_pio_add_program
#undef hub75_pio_config_xfer
#undef hub75_pio_init_sm
#undef hub75_pio_clear_fifos
#undef hub75_pio_set_enabled
#undef hub75_pio_xfer
#undef hub75_pio_tx_fifo_empty
#undef hub75_pio_remove_program
#undef hub75_pio_release


TEST_CASE("Hub75PioCApi: a failed open returns the errno, and caches nothing") {
    lib = Piolib{};
    lib.openErr = EACCES;
    int err = 0;
    CHECK(shim_hub75_pio_open(&err) == nullptr);
    CHECK(err == EACCES);   // the positive errno, from piolib's error pointer
    lib.openErr = EINVAL;
    CHECK(shim_hub75_pio_open(&err) == nullptr);
    CHECK(err == EINVAL);
    CHECK(shim_hub75_pio_open(nullptr) == nullptr);   // a caller that wants no code takes none
    CHECK(lib.opens == 3);   // each try reached piolib: a failure is not remembered
}

TEST_CASE("Hub75PioCApi: the handle is opened once for the process, and made current each time") {
    lib = Piolib{};
    int err = 0;
    void* first = shim_hub75_pio_open(&err);
    REQUIRE(first != nullptr);
    CHECK(lib.opens == 1);
    void* second = shim_hub75_pio_open(&err);
    CHECK(second == first);
    CHECK(lib.opens == 1);   // a second pio_open(0) would return -EBUSY, so the first handle is kept
    CHECK(callsOf("pio_select").size() == 2);   // and each open makes it the thread's current instance
    CHECK(callsOf("fatal_off").size() == 2);    // with piolib's errors returned, not fatal
}

TEST_CASE("Hub75PioCApi: a claim takes any free state machine, and passes a failure through, before a DMA ring was configured") {
    lib = Piolib{};
    void* pio = shim_hub75_pio_open(nullptr);
    lib.unusedSm = 2;
    CHECK(shim_hub75_pio_claim_sm(pio) == 2);
    lib.unusedSm = -EBUSY;
    CHECK(shim_hub75_pio_claim_sm(pio) == -EBUSY);
    CHECK(callsOf("claim ").empty());   // no preference to try: the SM claim call is never made
}

TEST_CASE("Hub75PioCApi: a ring that fails to configure is not remembered, one that configures is") {
    lib = Piolib{};
    void* pio = shim_hub75_pio_open(nullptr);
    lib.xferConfigRc = -EBUSY;
    CHECK(shim_hub75_pio_config_xfer(pio, 3, 8192, 4) == -EBUSY);
    lib.unusedSm = 1;
    lib.calls.clear();
    CHECK(shim_hub75_pio_claim_sm(pio) == 1);
    CHECK(callsOf("claim ").empty());   // nothing remembered: the failed configure of SM 3 left no preference

    lib.xferConfigRc = 0;
    CHECK(shim_hub75_pio_config_xfer(pio, 2, 8192, 4) == 0);   // SM 2's DMA is this process's now
    lib.calls.clear();
    CHECK(shim_hub75_pio_claim_sm(pio) == 2);   // the next claim takes it back, so its DMA claim is reused
    CHECK(callsOf("claim 2").size() == 1);
    CHECK(callsOf("claim_unused").empty());
}

TEST_CASE("Hub75PioCApi: the remembered state machine is skipped when another client holds it, or refuses the claim") {
    lib = Piolib{};
    void* pio = shim_hub75_pio_open(nullptr);
    lib.xferConfigRc = 0;
    REQUIRE(shim_hub75_pio_config_xfer(pio, 2, 8192, 4) == 0);

    lib.claimedMask = 1u << 2;   // another process took SM 2
    lib.unusedSm = 0;
    lib.calls.clear();
    CHECK(shim_hub75_pio_claim_sm(pio) == 0);   // any free one; the old DMA claim stays held until exit
    CHECK(callsOf("claim 2").empty());

    lib.claimedMask = 0;   // free by the claim bits, but the claim call itself fails
    lib.claimFails = true;
    lib.unusedSm = 3;
    lib.calls.clear();
    CHECK(shim_hub75_pio_claim_sm(pio) == 3);
    CHECK(callsOf("claim 2").size() == 1);   // tried, then fell back
    CHECK_FALSE(lib.errorFlag);              // and left no stale error behind

    lib.claimFails = false;
    REQUIRE(shim_hub75_pio_config_xfer(pio, 3, 8192, 4) == 0);   // a new SM's ring becomes the preference
    lib.calls.clear();
    CHECK(shim_hub75_pio_claim_sm(pio) == 3);
    CHECK(callsOf("claim 3").size() == 1);
}

TEST_CASE("Hub75PioCApi: a program load maps both of piolib's refusals to -ENOSPC, and gives the offset otherwise") {
    lib = Piolib{};
    void* pio = shim_hub75_pio_open(nullptr);
    const uint16_t program[3] = {1, 2, 3};
    lib.addOffset = 5;
    CHECK(shim_hub75_pio_add_program(pio, program, 3) == 5);
    lib.addOffset = PIO_ORIGIN_INVALID;   // piolib's answer when there is no room
    CHECK(shim_hub75_pio_add_program(pio, program, 3) == -ENOSPC);
    lib.addOffset = 0;
    lib.addFlagsError = true;   // or its error flag, which carries no errno
    CHECK(shim_hub75_pio_add_program(pio, program, 3) == -ENOSPC);
}

TEST_CASE("Hub75PioCApi: the state machine init makes the instance current first, and starts at the program's offset") {
    lib = Piolib{};
    void* pio = shim_hub75_pio_open(nullptr);
    hub75_pio_sm_params params{};
    params.offset = 7;
    params.wrap = 2;
    params.clkdiv_int = 4;
    lib.calls.clear();
    shim_hub75_pio_init_sm(pio, 1, &params);
    REQUIRE(!lib.calls.empty());
    CHECK(lib.calls.front() == "pio_select");   // the config calls read the thread's current instance
    CHECK(lib.initPc == 7);
}

#endif
