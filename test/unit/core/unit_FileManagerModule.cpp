/// @module FileManagerModule

/// Drives the file-manager create/delete ops against the real platform::fs* seam, isolated to a temp dir via fsSetRoot (the FilesystemModule-test pattern). The mkdir/delete ops are HTTP endpoints (POST/DELETE /api/dir?path=) whose handlers do parseFilePath(query) → fsMkdir/fsRemove; the HTTP framing needs a socket fixture (backlogged), so here we exercise the same seam contract the handler runs on, the create/delete behavior + robustness (non-empty-dir / '..' traversal), plus HttpServerModule::parseFilePath directly (it's pure string→string, so it needs no socket).

#include "doctest.h"
#include "core/system/FileManagerModule.h"
#include "core/system/HttpServerModule.h"   // parseFilePath — the shared filesystem-path guard
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace mm;

namespace {

// Write a seed file, failing the test cleanly if the handle can't be opened (never fputs/fclose a null FILE*, which would crash the whole binary).
void writeFile(const std::string& path, const char* contents) {
    FILE* f = std::fopen(path.c_str(), "w");
    REQUIRE(f != nullptr);
    std::fputs(contents, f);
    std::fclose(f);
}

// A file-manager on a fresh temp filesystem root. Seeds a known layout the ops act against.
struct Rig {
    char root[256];
    FileManagerModule fm;
    Rig() {
        // A monotonic per-process counter, not millis(), several Rigs construct within the same millisecond in one test run, so a time-based name could collide.
        static unsigned counter = 0;
        // temp_directory_path() is the portable temp root (/tmp on POSIX, %TEMP% on Windows), not a hardcoded "/tmp"; the counter keeps each Rig's dir unique within the run.
        std::snprintf(root, sizeof(root), "%s/mm_fm_test_%u",
                      std::filesystem::temp_directory_path().string().c_str(), counter++);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(std::string(root) + "/.config");
        writeFile(std::string(root) + "/.config/Drivers.json", "{\"brightness\":20}");
        writeFile(std::string(root) + "/readme.txt", "hello");
        platform::fsSetRoot(root);
        fm.setTypeName("FileManagerModule");
        fm.defineControls();
        fm.setup();
    }
    // Restore the DEFAULT root (whatever platform.h's fsSetRoot contract resolves "" to; under ctest that is the pinned MM_DATA_DIR), not ".", so a later test in the same binary starts from the same baseline this Rig assumed, never a leaked "." repo-root.
    // Teardown must never propagate: this Rig is destroyed while the stack unwinds from a failed CHECK, and a throw there terminates the process, losing the very failure being reported.
    // Hence both the error_code overload of remove_all (which cannot throw) and noexcept.
    // The only residual throw path is fsSetRoot's std::filesystem::path assignment (a theoretical bad_alloc on a short literal). noexcept turning that into terminate is the right trade here: a test rig that cannot reset the fs root must not limp on.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Rig() noexcept { platform::fsSetRoot(""); std::error_code ec; std::filesystem::remove_all(root, ec); }

    bool onDisk(const char* rel) const {
        return std::filesystem::exists(std::string(root) + rel);
    }
};

// The mkdir op (POST /api/dir?path=), sans HTTP: the handler is fsMkdir on the guarded path. Returns the seam result. fsSetRoot has already rooted the temp dir, so a plain rel path maps in.
bool mkdirOp(const char* rel) { return platform::fsMkdir(rel); }
// The delete op (DELETE /api/dir?path=): fsRemove a file or EMPTY dir.
bool deleteOp(const char* rel) { return platform::fsRemove(rel); }

}  // namespace

// The upload path writes through these, and a first file into a folder that does not exist yet answered "write failed" on a device.
TEST_CASE("A file written into a folder that does not exist yet creates the folder, through both writes") {
    Rig rig;
    REQUIRE(platform::fsWriteAtomic("/fresh/a/one.txt", "1", 1));
    CHECK(platform::fsExists("/fresh/a/one.txt"));
    const char* text = "22";
    struct Src { const char* p; size_t left; };
    Src src{text, 2};
    const auto pull = [](char* buf, size_t cap, void* user, bool*) -> size_t {
        auto* s = static_cast<Src*>(user);
        const size_t n = s->left < cap ? s->left : cap;
        std::memcpy(buf, s->p, n);
        s->p += n;
        s->left -= n;
        return n;
    };
    REQUIRE(platform::fsWriteStream("/fresh/b/two.txt", pull, &src));
    CHECK(platform::fsExists("/fresh/b/two.txt"));
}

TEST_CASE("FileManager: mkdir creates a dir at the target path; delete removes it") {
    Rig r;
    CHECK(mkdirOp("/newdir"));
    CHECK(r.onDisk("/newdir"));
    CHECK(deleteOp("/newdir"));
    CHECK(!r.onDisk("/newdir"));
}

TEST_CASE("FileManager: mkdir nested under an existing dir") {
    Rig r;
    CHECK(mkdirOp("/.config/sub"));
    CHECK(r.onDisk("/.config/sub"));
}

TEST_CASE("FileManager: delete of a non-empty folder is rejected, not a crash") {
    Rig r;
    CHECK(!deleteOp("/.config"));       // .config holds Drivers.json → not empty → fails cleanly
    CHECK(r.onDisk("/.config"));        // still there
}

TEST_CASE("FileManager: delete removes a file too") {
    Rig r;
    CHECK(deleteOp("/readme.txt"));
    CHECK(!r.onDisk("/readme.txt"));
}

TEST_CASE("FileManager: a '..' traversal never escapes root (the seam's confinement)") {
    Rig r;
    // fsSetRoot confines the seam to the temp root; even a raw '..' can't create outside it. The HTTP handler additionally rejects '..' up front in parseFilePath before reaching the seam (below).
    (void)mkdirOp("/../escape");
    CHECK(!std::filesystem::exists(std::string(r.root) + "/../escape"));   // nothing outside root
}

// parseFilePath is the single path guard every filesystem HTTP entry (read/write/dir/mkdir/delete) runs on, pure string→string, so it's tested directly here without a socket. It decodes the `path=` query value (%XX + '+'), roots a relative path at the mount, and rejects a missing/empty path, a `..` traversal (raw OR percent-encoded), and an overlong (buffer-filling) value.
TEST_CASE("HttpServer::parseFilePath accepts a valid path and roots a relative one") {
    char out[64];
    // Absolute path passes through as-is.
    CHECK(HttpServerModule::parseFilePath("path=/foo/bar.json", out, sizeof(out)));
    CHECK(std::strcmp(out, "/foo/bar.json") == 0);
    // Relative path is rooted at the mount ('/').
    CHECK(HttpServerModule::parseFilePath("path=readme.txt", out, sizeof(out)));
    CHECK(std::strcmp(out, "/readme.txt") == 0);
    // %XX + '+' decode: "%2Ffoo bar" → "/foo bar".
    CHECK(HttpServerModule::parseFilePath("path=%2Ffoo+bar", out, sizeof(out)));
    CHECK(std::strcmp(out, "/foo bar") == 0);
    // The path stops at the '&' delimiter (further query params are ignored).
    CHECK(HttpServerModule::parseFilePath("path=/a.json&hidden=1", out, sizeof(out)));
    CHECK(std::strcmp(out, "/a.json") == 0);
}

TEST_CASE("HttpServer::parseFilePath rejects traversal, empty, missing, and overlong") {
    char out[64];
    // Raw '..' anywhere → reject.
    CHECK_FALSE(HttpServerModule::parseFilePath("path=/../etc/passwd", out, sizeof(out)));
    CHECK_FALSE(HttpServerModule::parseFilePath("path=foo/../../bar", out, sizeof(out)));
    // Percent-encoded '..' (%2e%2e) decodes to '..' BEFORE the check → also reject.
    CHECK_FALSE(HttpServerModule::parseFilePath("path=%2e%2e/secret", out, sizeof(out)));
    // Missing path= key, and an empty value → reject.
    CHECK_FALSE(HttpServerModule::parseFilePath("hidden=1", out, sizeof(out)));
    CHECK_FALSE(HttpServerModule::parseFilePath("path=", out, sizeof(out)));
    CHECK_FALSE(HttpServerModule::parseFilePath(nullptr, out, sizeof(out)));
    // Overlong value (fills the buffer before the terminator) → reject, not a truncated prefix.
    char small[8];
    CHECK_FALSE(HttpServerModule::parseFilePath("path=/this/is/way/too/long", small, sizeof(small)));
}

// The /api/file upload streams the body to fsWriteStream (any size, binary-safe) and downloads via fsReadAt (positional, chunked). These pin the seam primitives that path relies on. (The HTTP framing itself needs a socket fixture, backlogged; here we exercise the platform contracts.)

// A multi-chunk source (larger than the src callback's cap) with a NUL writes in full and reads back byte-for-byte, the streamed-upload contract.
namespace {
struct SpanSrc { const char* p; size_t left; };
// The signature must match the FsWriteSrc typedef, where `abort` is an out-parameter a source sets to stop the stream early, so it cannot become a pointer-to-const. NOLINTNEXTLINE(readability-non-const-parameter)
size_t spanPull(char* out, size_t cap, void* user, bool* abort) {
    (void)abort;   // this source always ends cleanly (no early close / timeout to signal)
    auto* s = static_cast<SpanSrc*>(user);
    const size_t n = s->left < cap ? s->left : cap;
    std::memcpy(out, s->p, n);
    s->p += n; s->left -= n;
    return n;   // 0 when exhausted → clean EOF
}
}  // namespace

TEST_CASE("FileManager: fsWriteStream writes a multi-chunk NUL-containing payload in full") {
    Rig r;
    // 3000 bytes (spans several 1KB internal chunks) with a NUL planted mid-stream.
    std::string payload(3000, 'X');
    payload[1500] = '\0';
    payload[2999] = 'Z';
    SpanSrc src{payload.data(), payload.size()};
    REQUIRE(platform::fsWriteStream("/blob.bin", &spanPull, &src));
    CHECK(platform::fsSize("/blob.bin") == static_cast<long>(payload.size()));

    // Read it back positionally across a chunk boundary, the streamed-download contract.
    char win[64] = {};
    const int got = platform::fsReadAt("/blob.bin", 1480, win, 40);   // straddles the NUL at 1500
    CHECK(got == 40);
    CHECK(win[20] == '\0');                                           // the planted NUL, offset 1500
    CHECK(std::memcmp(win, payload.data() + 1480, 40) == 0);
}

// A source that reports a short read (mid-stream failure) still commits atomically, here a clean end just yields the bytes delivered; there's no partial/torn file (temp → rename).
TEST_CASE("FileManager: fsWriteStream commits atomically (no torn file)") {
    Rig r;
    const char data[] = {'a', 'b', '\0', 'c', 'd'};
    SpanSrc src{data, sizeof(data)};
    REQUIRE(platform::fsWriteStream("/atomic.bin", &spanPull, &src));
    char back[16] = {};
    const int n = platform::fsRead("/atomic.bin", back, sizeof(back));
    CHECK(n == static_cast<int>(sizeof(data)));
    CHECK(std::memcmp(back, data, sizeof(data)) == 0);
    // No leftover temp file beside it.
    CHECK(!std::filesystem::exists(std::string(r.root) + "/atomic.bin.tmp"));
}

// A source that aborts mid-stream (an incomplete/timed-out upload) must NOT commit, fsWriteStream discards the temp and returns false, so a truncated body never lands as a real file.
TEST_CASE("FileManager: fsWriteStream discards on abort (incomplete upload)") {
    Rig r;
    struct AbortSrc { int calls = 0; };
    AbortSrc a;
    auto src = [](char* out, size_t cap, void* user, bool* abort) -> size_t {
        auto* s = static_cast<AbortSrc*>(user);
        if (s->calls++ == 0) { const size_t n = cap < 3 ? cap : 3; std::memcpy(out, "abc", n); return n; }
        *abort = true; return 0;   // second call: the rest never arrived
    };
    CHECK(!platform::fsWriteStream("/partial.bin", src, &a));   // aborted → false
    CHECK(platform::fsSize("/partial.bin") < 0);                // no file committed
    CHECK(!std::filesystem::exists(std::string(r.root) + "/partial.bin.tmp"));   // temp discarded
}

TEST_CASE("HTTP header names match case-insensitively, so any client's Content-Length counts") {
    // The bench-found wipe: node's undici sends `content-length:` lowercase; the case-sensitive strstr read "no length declared", and an upload committed an EMPTY file with a 200, a silent config wipe. RFC 9112 makes field names case-insensitive; the finder must too.
    const char* req = "POST /api/file?path=/x HTTP/1.1\r\ncontent-length: 831\r\n\r\nbody";
    const char* hit = mm::HttpServerModule::findHeaderCI(req, "Content-Length:");
    REQUIRE(hit != nullptr);
    CHECK(std::strncmp(hit, "content-length:", 15) == 0);

    // Canonical and mixed casings resolve to the same header.
    CHECK(mm::HttpServerModule::findHeaderCI("Content-Length: 5\r\n", "content-length:") != nullptr);
    CHECK(mm::HttpServerModule::findHeaderCI("CONTENT-LENGTH: 5\r\n", "Content-Length:") != nullptr);
    // And a request without the header still reads as absent.
    CHECK(mm::HttpServerModule::findHeaderCI("GET / HTTP/1.1\r\nHost: x\r\n", "Content-Length:") == nullptr);

    // Anchored at line starts: an X-prefixed lookalike is NOT the header.
    CHECK(mm::HttpServerModule::findHeaderCI("POST / HTTP/1.1\r\nX-Content-Length: 9\r\n\r\n", "Content-Length:") == nullptr);
    // Bounded by the blank line: a header name inside the BODY is data, not a header.
    CHECK(mm::HttpServerModule::findHeaderCI("POST / HTTP/1.1\r\nHost: x\r\n\r\nContent-Length: 4", "Content-Length:") == nullptr);
}

// removeRecursive: the DELETE /api/dir path, exercised directly rather than through a socket.
//
// It is public for exactly this, and until now nothing called it: the header claimed the tests exercised the real recursion while none referenced it. These are the behaviors a user reaches by deleting a folder from the File Manager.
TEST_CASE("removeRecursive deletes a folder and everything under it") {
    Rig r;
    std::filesystem::create_directories(std::string(r.root) + "/tree/a/b");
    writeFile(std::string(r.root) + "/tree/top.txt", "1");
    writeFile(std::string(r.root) + "/tree/a/mid.txt", "2");
    writeFile(std::string(r.root) + "/tree/a/b/leaf.txt", "3");

    CHECK(mm::HttpServerModule::removeRecursive("/tree"));
    CHECK_FALSE(r.onDisk("/tree"));
}

// The depth bound is what keeps a user-shaped tree from running the stack out. A tree deeper than the bound is REFUSED rather than half-deleted: reporting failure lets the caller delete again and take the next batch, which is the same contract the width cap (DirLevel::kMax) has.
TEST_CASE("removeRecursive refuses a tree deeper than its bound") {
    Rig r;
    std::string deep = std::string(r.root) + "/deep";
    for (int i = 0; i < 12; i++) deep += "/x";        // past the depth-8 bound
    std::filesystem::create_directories(deep);

    CHECK_FALSE(mm::HttpServerModule::removeRecursive("/deep"));
    CHECK(r.onDisk("/deep"));                         // still there, not partly gone
}

// A single file, which is the case that returns on the first fsRemove without ever listing.
TEST_CASE("removeRecursive deletes a plain file") {
    Rig r;
    CHECK(r.onDisk("/readme.txt"));
    CHECK(mm::HttpServerModule::removeRecursive("/readme.txt"));
    CHECK_FALSE(r.onDisk("/readme.txt"));
}
