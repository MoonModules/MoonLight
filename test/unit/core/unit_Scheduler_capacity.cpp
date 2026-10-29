/// @module Scheduler

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"

#include <cstdio>
#include <string>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

// Pins Scheduler::addModule at its limit. A boot that wires past the top-level array must learn of it, so the add answers false and logs a line rather than dropping the module silently.

namespace {
struct Stub : public mm::MoonModule {};

#ifdef _WIN32
int dupFd(int fd) { return _dup(fd); }
int dup2Fd(int from, int to) { return _dup2(from, to); }
int closeFd(int fd) { return _close(fd); }
int fdOf(std::FILE* f) { return _fileno(f); }
#else
int dupFd(int fd) { return dup(fd); }
int dup2Fd(int from, int to) { return dup2(from, to); }
int closeFd(int fd) { return close(fd); }
int fdOf(std::FILE* f) { return fileno(f); }
#endif

// What a span of code prints to stdout: stdout points at a temporary file from construction until text(), then points back.
struct StdoutCapture {
    std::FILE* file = std::tmpfile();
    int saved = -1;

    StdoutCapture() {
        if (!file) return;
        std::fflush(stdout);
        saved = dupFd(fdOf(stdout));
        dup2Fd(fdOf(file), fdOf(stdout));
    }
    std::string text() {
        restore();
        std::string out;
        if (!file) return out;
        std::rewind(file);
        char buf[256];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, file)) > 0;) out.append(buf, n);
        return out;
    }
    ~StdoutCapture() {
        restore();
        if (file) std::fclose(file);
    }

private:
    void restore() {
        if (saved < 0) return;
        std::fflush(stdout);
        dup2Fd(saved, fdOf(stdout));
        closeFd(saved);
        saved = -1;
    }
};
} // namespace

// The 33rd add is refused, answers false and prints which module had no room and the limit, while the first 32 are accepted and stay in order.
TEST_CASE("Scheduler::addModule refuses a module past the limit and says so") {
    mm::Scheduler s;
    Stub* mods[33];
    for (int i = 0; i < 33; i++) {
        mods[i] = new Stub();
        mods[i]->setName(i < 32 ? "M" : "Extra");
    }
    for (int i = 0; i < 32; i++) CHECK(s.addModule(mods[i]));

    bool added = true;
    std::string said;
    {
        StdoutCapture out;   // around the refused add alone, so no check's own output lands in it
        added = s.addModule(mods[32]);
        said = out.text();
    }
    CHECK_FALSE(added);
    CHECK(said.find("no room for module Extra") != std::string::npos);
    CHECK(said.find("32 top-level modules is the limit") != std::string::npos);

    for (int i = 0; i < 32; i++) CHECK(s.module(i) == mods[i]);
    CHECK(s.module(32) == nullptr);

    CHECK_FALSE(s.addModule(nullptr));
    for (int i = 0; i < 33; i++) {
        if (i < 32) s.deleteTree(mods[i]);
        else delete mods[i];
    }
}
