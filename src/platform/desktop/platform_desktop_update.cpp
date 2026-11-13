// SPDX-License-Identifier: GPL-3.0-or-later
/// A desktop installing a new version of itself as a board installs an image into its other slot.
///
/// The image goes to `MoonLight.next` beside the running program; `otaEnd` vets it, keeps the running program as `MoonLight.prev` and renames the new one in.
///
/// The restart runs it: the pattern of the self-update libraries (go-update, Rust's self_update). A container is its host's to replace.

#include "host_update.h"
#include "platform/platform.h"
#include "ProgramImage.h"             // the header a new program is vetted by, as a board vets its image's
#include "core/util/hex.h"            // isHexDigits: a digest's form
#include "core/util/sha256.h"         // the digest a fetch is checked against

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>      // pthread_sigmask: the new program starts with every signal open
#include <unistd.h>     // access, execv, geteuid
#endif
#ifdef __APPLE__
#include <crt_externs.h>    // _NSGetArgc, _NSGetArgv: the command line the restart repeats
#include <mach-o/dyld.h>    // _NSGetExecutablePath
#endif

namespace fs = std::filesystem;

namespace mm::platform {

namespace {

fs::path g_testProgram;   // a test's stand-in for the running program, empty for the real one

/// The largest image a desktop takes, some 80 times its program: a URL serving more fills the disk before its header could refuse it.
constexpr uint32_t kMaxProgramBytes = 256u * 1024 * 1024;

/// The system and processor this build runs on, which a new program must be built for.
constexpr ProgramSystem kThisSystem =
#if defined(_WIN32)
    ProgramSystem::Windows;
#elif defined(__APPLE__)
    ProgramSystem::MacOS;
#else
    ProgramSystem::Linux;
#endif
constexpr ProgramCpu kThisCpu =
#if defined(__aarch64__) || defined(_M_ARM64)
    ProgramCpu::Arm64;
#else
    ProgramCpu::X64;
#endif

void say(char* status, size_t statusLen, const char* text) {
    if (status && statusLen) std::snprintf(status, statusLen, "%s", text);
}

std::FILE* openFile(const fs::path& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wideMode(mode, mode + std::strlen(mode));
    return ::_wfopen(path.c_str(), wideMode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

fs::path withSuffix(const fs::path& path, const char* suffix) {
    fs::path suffixed = path;
    suffixed += suffix;
    return suffixed;
}

/// The file's first bytes, where every executable format keeps its header.
std::vector<uint8_t> readHead(const fs::path& path) {
    std::vector<uint8_t> head(4096);
    std::FILE* file = openFile(path, "rb");
    head.resize(file ? std::fread(head.data(), 1, head.size(), file) : 0);
    if (file) std::fclose(file);
    return head;
}

/// Whether the file's SHA-256 is the one the release names, compared case-insensitively.
bool digestMatches(const fs::path& path, const char* expectedHex) {
    if (!isHexDigits(expectedHex, kSha256DigestSize * 2)) return false;
    std::error_code ec;
    const auto fileBytes = fs::file_size(path, ec);
    if (ec) return false;
    std::FILE* file = openFile(path, "rb");
    if (!file) return false;
    std::vector<char> bytes(static_cast<size_t>(fileBytes));
    const bool readWhole = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    if (!readWhole) return false;
    char actualHex[kSha256DigestSize * 2 + 1];
    sha256Hex(bytes.data(), bytes.size(), actualHex, kSha256DigestSize);
    for (size_t i = 0; i < kSha256DigestSize * 2; i++)
        if (actualHex[i] != std::tolower(static_cast<unsigned char>(expectedHex[i]))) return false;
    return true;
}

/// Whether this process may write files in `folder`, which is whether it can put a new program there.
bool folderWritable(const fs::path& folder) {
#ifdef _WIN32
    // Windows' access check reads only the read-only flag, so a probe file asks the file system itself.
    const fs::path probe = folder / "MoonLight.probe";
    HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
#else
    return ::access(folder.c_str(), W_OK) == 0;
#endif
}

/// Put `next` at `program`, the running one kept as `.prev`; the error to report, or nullptr once swapped.
const char* swapIn(const fs::path& next, const fs::path& program) {
    std::error_code ec;
    const fs::path prev = withSuffix(program, ".prev");
    fs::remove(prev, ec);
#ifdef _WIN32
    // A running program cannot be overwritten on Windows, but it can be renamed: the old one steps aside and the new one takes its name.
    fs::rename(program, prev, ec);
    if (ec) return "error: cannot keep the running program as .prev";
    fs::rename(next, program, ec);
    if (ec) {
        std::error_code restored;
        fs::rename(prev, program, restored);   // the old program back under its name
    }
#else
    fs::permissions(next, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec, ec);
    // The running program keeps a second name, so the rollback has it to put back and the program's own name holds a whole program at every moment.
    fs::create_hard_link(program, prev, ec);
    if (ec) {   // a file system without hard links (FAT, exFAT, SMB) keeps a copy instead
        ec.clear();
        fs::copy_file(program, prev, ec);
    }
    if (ec) return "error: cannot keep the running program as .prev";
    fs::rename(next, program, ec);   // rename(2), atomic: the name moves from the old program to the new one in one step
#endif
    return ec ? "error: cannot put the new program in place" : nullptr;
}

/// An image on its way to `MoonLight.next`, which `otaEnd` puts in place.
struct ProgramWrite {
    fs::path next;
    fs::path program;
    std::FILE* file = nullptr;
    char* status = nullptr;
    size_t statusLen = 0;
    uint32_t* bytesOut = nullptr;
    uint32_t expected = 0;   ///< the image's size once known, for the progress line
    const uint32_t* total = nullptr;   ///< a download's size as its headers name it, where `expected` is unknown up front
    uint32_t written = 0;
    std::string sha256;      ///< the digest a fetch names, empty for an upload
};

/// The progress line, "of N" once the image's size is known, from the upload's length or the download's headers.
void sayProgress(const ProgramWrite& img) {
    if (!img.status || !img.statusLen) return;
    const uint32_t expected = img.expected ? img.expected : (img.total ? *img.total : 0);
    if (expected) std::snprintf(img.status, img.statusLen, "flashing: %u of %u bytes", static_cast<unsigned>(img.written), static_cast<unsigned>(expected));
    else std::snprintf(img.status, img.statusLen, "flashing: %u bytes", static_cast<unsigned>(img.written));
}

/// Drop an image that will not be installed.
void abandon(OtaWrite& w) {
    std::unique_ptr<ProgramWrite> img(static_cast<ProgramWrite*>(w.impl));
    w.impl = nullptr;
    if (!img) return;
    if (img->file) std::fclose(img->file);
    std::error_code ec;
    fs::remove(img->next, ec);
}

#ifndef _WIN32
/// The command line this run started with, to start the next one the same way.
std::vector<std::string> commandLine() {
    std::vector<std::string> args;
#ifdef __APPLE__
    args.reserve(static_cast<size_t>(*_NSGetArgc()));
    for (int i = 0; i < *_NSGetArgc(); i++) args.emplace_back((*_NSGetArgv())[i]);
#else
    std::FILE* file = std::fopen("/proc/self/cmdline", "rb");
    std::string arg;
    for (int c; file && (c = std::fgetc(file)) != EOF;) {
        if (c) arg += static_cast<char>(c);
        else { args.push_back(arg); arg.clear(); }
    }
    if (file) std::fclose(file);
#endif
    return args;
}
#endif

/// The fetch's own thread: the image downloaded into the writer, put in place, and the program restarted on it.
void fetchTask(const std::string& url, const std::string& fallbackUrl, const std::string& sha256Hex, char* status, size_t statusLen,
               uint32_t* bytesRead, uint32_t* bytesTotal) {
    const std::string* const sources[] = {&url, &fallbackUrl};
    for (const std::string* from : sources) {
        if (from->empty()) continue;
        OtaWrite w;
        if (!otaBegin(w, OtaTarget::App, 0, status, statusLen, bytesRead)) return;
        static_cast<ProgramWrite*>(w.impl)->sha256 = sha256Hex;
        static_cast<ProgramWrite*>(w.impl)->total = bytesTotal;
        const int httpStatus = httpDownload(from->c_str(), 10 * 60 * 1000,
                                            [](void* user, const char* data, size_t n) {
                                                return otaWrite(*static_cast<OtaWrite*>(user), reinterpret_cast<const uint8_t*>(data), n);
                                            },
                                            &w, bytesTotal);
        if (httpStatus != 200) {
            if (w.impl) abandon(w);
            // The update stays in flight while the other address is still to try, so no second update starts on MoonLight.next meanwhile.
            say(status, statusLen, from == &url && !fallbackUrl.empty() ? "downloading" : "error: the download failed");
            continue;
        }
        if (otaEnd(w, true)) {
            say(status, statusLen, "rebooting");
            reboot();
        }
        return;   // refused by its header or digest: the other address serves the same file
    }
}

/// The running program's file as the system names it, read once at start: once a new program takes the name, Linux names the running one "... (deleted)".
fs::path executablePath() {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring path(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return fs::path(path);
#elif defined(__APPLE__)
    std::string path(4096, '\0');
    uint32_t size = static_cast<uint32_t>(path.size());
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    return fs::weakly_canonical(fs::path(path.c_str()), ec);
#else
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}
}  // namespace

fs::path programPath() {
    static const fs::path installed = executablePath();
    return g_testProgram.empty() ? installed : g_testProgram;
}

void setTestProgramPath(const fs::path& path) { g_testProgram = path; }

UpdateMode updateMode() {
    if (inContainer()) return UpdateMode::Host;
    const auto modeOf = [](const fs::path& program) {
#ifndef _WIN32
        // Root writes every folder, the package's seed among them, so a program run as root is updated by hand, never on a request from the network.
        if (::geteuid() == 0) return UpdateMode::Manual;
#endif
        return !program.empty() && folderWritable(program.parent_path()) ? UpdateMode::Self : UpdateMode::Manual;
    };
    if (!g_testProgram.empty()) return modeOf(g_testProgram);
    static const UpdateMode installed = modeOf(programPath());   // once, as the Firmware card shows it: Windows asks its folder with a probe file
    return installed;
}

bool putProgramInPlace(const fs::path& next, const fs::path& program, const char* sha256Hex, char* status, size_t statusLen) {
    char why[96];
    const auto refuse = [&](const char* reason) {
        std::error_code ignored;
        fs::remove(next, ignored);
        say(status, statusLen, reason);
        return false;
    };
    if (const char* rejection = programRejection(readHead(next), kThisSystem, kThisCpu)) {
        std::snprintf(why, sizeof(why), "error: the image is %s", rejection);
        return refuse(why);
    }
    if (sha256Hex && *sha256Hex && !digestMatches(next, sha256Hex)) return refuse("error: the image does not match the release's digest");
    // Pending until the new program has run a minute, which is when the service's rollback stops applying to it; marked first, so no new program runs unmarked.
    const fs::path pending = withSuffix(program, ".pending");
    std::FILE* marker = openFile(pending, "wb");
    if (!marker) return refuse("error: cannot mark the new program pending");
    std::fclose(marker);
    if (const char* failure = swapIn(next, program)) {
        std::error_code ignored;
        fs::remove(pending, ignored);
        return refuse(failure);
    }
    return true;
}

void confirmProgramWhenStable() {
    const fs::path pending = withSuffix(programPath(), ".pending");
    std::error_code ec;
    if (!fs::exists(pending, ec)) return;
    std::thread([pending] {
        std::this_thread::sleep_for(std::chrono::milliseconds(kBootStableMs));
        std::error_code ignored;
        fs::remove(pending, ignored);
    }).detach();
}

bool otaBegin(OtaWrite& w, OtaTarget target, size_t contentLen, char* statusBuf, size_t statusBufLen, uint32_t* bytesOut) {
    if (bytesOut) *bytesOut = 0;
    if (target != OtaTarget::App) {
        say(statusBuf, statusBufLen, "error: a desktop has one program, no MoonBase or radio chip");
        return false;
    }
    if (updateMode() != UpdateMode::Self) {
        say(statusBuf, statusBufLen, "error: this program is updated by its host or by hand");
        return false;
    }
    if (contentLen > kMaxProgramBytes) {
        say(statusBuf, statusBufLen, "error: the image is too large for a program");
        return false;
    }
    auto img = std::unique_ptr<ProgramWrite>(new (std::nothrow) ProgramWrite);
    if (!img) { say(statusBuf, statusBufLen, "error: out of memory"); return false; }
    img->program = programPath();
    img->next = img->program.parent_path() / "MoonLight.next";
    img->status = statusBuf;
    img->statusLen = statusBufLen;
    img->bytesOut = bytesOut;
    img->expected = static_cast<uint32_t>(contentLen);
    img->file = openFile(img->next, "wb");
    if (!img->file) { say(statusBuf, statusBufLen, "error: cannot write beside the program"); return false; }
    say(statusBuf, statusBufLen, "flashing");
    w.impl = img.release();
    return true;
}

bool otaWrite(OtaWrite& w, const uint8_t* data, size_t len) {
    auto* img = static_cast<ProgramWrite*>(w.impl);
    if (!img) return false;
    const char* failure = len > kMaxProgramBytes - img->written ? "error: the image is too large for a program"
                          : std::fwrite(data, 1, len, img->file) != len ? "error: cannot write the new program"
                                                                       : nullptr;
    if (failure) {
        say(img->status, img->statusLen, failure);
        abandon(w);
        return false;
    }
    img->written += static_cast<uint32_t>(len);
    if (img->bytesOut) *img->bytesOut = img->written;
    sayProgress(*img);
    return true;
}

bool otaEnd(OtaWrite& w, bool commit) {
    auto* img = static_cast<ProgramWrite*>(w.impl);
    if (!img) return false;
    const bool closed = std::fclose(img->file) == 0;
    img->file = nullptr;
    if (!commit || !closed) {
        if (commit) say(img->status, img->statusLen, "error: cannot write the new program");
        abandon(w);
        return false;
    }
    const std::unique_ptr<ProgramWrite> owned(img);
    w.impl = nullptr;
    return putProgramInPlace(owned->next, owned->program, owned->sha256.c_str(), owned->status, owned->statusLen);
}

bool http_fetch_to_ota(const char* url, char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut,
                       uint32_t* bytesTotalOut, const char* fallbackUrl, const char* sha256Hex) {
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    if (!url || !statusBuf || statusBufLen == 0) return false;
    if (updateMode() != UpdateMode::Self) {
        say(statusBuf, statusBufLen, "error: this program is updated by its host or by hand");
        return false;
    }
    say(statusBuf, statusBufLen, "downloading");
    std::thread(fetchTask, std::string(url), std::string(fallbackUrl ? fallbackUrl : ""), std::string(sha256Hex ? sha256Hex : ""),
                statusBuf, statusBufLen, bytesReadOut, bytesTotalOut).detach();
    return true;
}

void reboot() {
    std::printf("platform::reboot(): restarting\n");
    std::fflush(stdout);
    const fs::path program = programPath();
#ifdef _WIN32
    // The new program starts in this console and takes the port once this one exits; a virus scan can hold a fresh program a moment, so it is retried.
    std::wstring commandLine = ::GetCommandLineW();
    if (commandLine.find(L"--no-browser") == std::wstring::npos) commandLine += L" --no-browser";
    for (int attempt = 0; attempt < 10; attempt++) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (::CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            std::exit(0);   // NOLINT(concurrency-mt-unsafe): the new program runs on
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::printf("platform::reboot(): could not start %ls (error %lu): start MoonLight again by hand\n", program.c_str(), ::GetLastError());
    std::fflush(stdout);
#else
    // The new program takes over this process in place, the one systemd or a container started, so their restart counting sees crashes only.
    std::vector<std::string> args = commandLine();
    if (args.empty()) args.push_back(program.string());
    bool quiet = false;
    for (const std::string& arg : args) quiet = quiet || arg == "--no-browser";
    if (!quiet) args.emplace_back("--no-browser");
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    // The sockets close on exec, the listening one among them (close_on_exec where each is made), so the new program binds the port again.
    sigset_t none;
    sigemptyset(&none);
    ::pthread_sigmask(SIG_SETMASK, &none, nullptr);   // a thread's blocked signals would carry over and leave Ctrl+C dead
    ::execv(program.c_str(), argv.data());
#endif
    std::exit(0);   // NOLINT(concurrency-mt-unsafe): on Windows the new program runs on; elsewhere exec failed, and exiting is what a reboot did before
}

}  // namespace mm::platform
