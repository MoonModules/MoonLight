/// @defgroup platform_esp32_fs LittleFS mount and the filesystem seam
/// The partition mount and the file API on an ESP32.
///
/// The file owns its private state and the path translation; the rest of the layer reaches it only through the declared symbols.
///
/// @moreinfo
///
/// ## Two partition labels, tried in order
///
/// The volume has always held LittleFS, and a table written from 2026-08 says so: the label and subtype both name it. An older table calls the same volume by the legacy misnomer, so both are tried and a device keeps its config across an update.
///
/// ## Formatting is gated on the LAST EXISTING label
///
/// A formatting mount against the wrong label would erase a volume the next label would have opened, taking the user's config with it.
/// Gating it on the last entry in the array rather than the last one present left a fresh board unformatted, running with persistence disabled.

#include "platform/platform.h"

#include "esp_littlefs.h"
#include "esp_partition.h"   // esp_partition_find_first: probing which FS label this table uses
#include "esp_log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mm::platform {

// LittleFS state
static constexpr const char* FS_TAG = "mm_fs";
// Both labels, in order: @xref{two-partition-labels-tried-in-order|why two}.
struct FsCandidate { esp_partition_subtype_t subtype; const char* label; };
static constexpr FsCandidate FS_CANDIDATES[] = {
    {ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "littlefs"},
    {ESP_PARTITION_SUBTYPE_DATA_SPIFFS,   "spiffs"},
};
static const char* fsLabelInUse_ = nullptr;
static constexpr const char* FS_MOUNT_POINT = "/littlefs";    // VFS mount point; not exposed in API paths
static bool fsMounted_ = false;

/// Map an API path onto the mount point; false on truncation, leaving nothing partial to consume.
static bool fsTranslate(const char* apiPath, char* out, size_t outLen) {
    if (outLen == 0) return false;
    if (!apiPath) { out[0] = 0; return false; }
    const char* sep = (apiPath[0] == '/') ? "" : "/";
    int n = std::snprintf(out, outLen, "%s%s%s", FS_MOUNT_POINT, sep, apiPath);
    if (n < 0 || static_cast<size_t>(n) >= outLen) { out[0] = 0; return false; }
    return true;
}

void fsSetRoot(const char* /*path*/) {
    // A no-op here, the mount point being fixed; it exists so a test can call it portably.
}

const char* fsRootPath() { return FS_MOUNT_POINT; }

bool fsMount() {
    if (fsMounted_) return true;

    // Formatting is enabled only on the last label that actually HAS a partition: @xref{formatting-is-gated-on-the-last-existing-label|why}.
    esp_err_t err = ESP_FAIL;
    constexpr size_t kCandidates = sizeof(FS_CANDIDATES) / sizeof(FS_CANDIDATES[0]);
    size_t lastPresent = kCandidates;
    for (size_t i = 0; i < kCandidates; i++) {
        const auto& c = FS_CANDIDATES[i];
        if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, c.subtype, c.label)) lastPresent = i;
    }
    for (size_t i = 0; i < kCandidates; i++) {
        const auto& c = FS_CANDIDATES[i];
        if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, c.subtype, c.label)) continue;
        esp_vfs_littlefs_conf_t conf = {};
        conf.base_path = FS_MOUNT_POINT;
        conf.partition_label = c.label;
        conf.format_if_mount_failed = (i == lastPresent);
        conf.dont_mount = false;
        err = esp_vfs_littlefs_register(&conf);
        if (err == ESP_OK) { fsLabelInUse_ = c.label; break; }
    }
    if (err != ESP_OK) {
        ESP_LOGE(FS_TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    fsMounted_ = true;
    ESP_LOGI(FS_TAG, "LittleFS mounted at %s (partition: %s)", FS_MOUNT_POINT, fsLabelInUse_);
    return true;
}

void fsUnmount() {
    if (!fsMounted_) return;
    if (fsLabelInUse_) esp_vfs_littlefs_unregister(fsLabelInUse_);
    fsMounted_ = false;
}

bool fsMkdir(const char* path) {
    if (!fsMounted_) return false;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    // mkdir -p: walk components, create each if missing
    char* p = full + std::strlen(FS_MOUNT_POINT) + 1; // skip "/littlefs/"
    while (*p) {
        if (*p == '/') {
            *p = 0;
            mkdir(full, 0775);  // ignore errors; could already exist
            *p = '/';
        }
        p++;
    }
    int rc = mkdir(full, 0775);
    return rc == 0 || errno == EEXIST;
}

bool fsExists(const char* path) {
    if (!fsMounted_) return false;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    struct stat st;
    return stat(full, &st) == 0;
}

bool fsRemove(const char* path) {
    if (!fsMounted_) return false;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    // Matching the desktop contract: remove maps to unlink here and fails on a directory, so stat first.
    struct stat st;
    if (::stat(full, &st) == 0 && S_ISDIR(st.st_mode)) return ::rmdir(full) == 0;
    return ::remove(full) == 0;
}

int fsRead(const char* path, char* buf, size_t maxLen) {
    if (!fsMounted_ || !buf || maxLen == 0) return -1;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return -1;
    FILE* f = std::fopen(full, "rb");
    if (!f) return -1;
    size_t n = std::fread(buf, 1, maxLen - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return static_cast<int>(n);
}

// The folder a write lands in, made first, as the write's contract promises.
static void makeParent(const char* path) {
    const char* slash = std::strrchr(path, '/');
    if (!slash || slash == path) return;
    char dir[128];
    const size_t n = static_cast<size_t>(slash - path);
    if (n >= sizeof(dir)) return;
    std::memcpy(dir, path, n);
    dir[n] = 0;
    fsMkdir(dir);
}

bool fsWriteAtomic(const char* path, const char* data, size_t len) {
    if (!fsMounted_) return false;
    // A non-zero length with a null pointer is undefined in the write below, so guard the boundary.
    if (len > 0 && !data) return false;
    makeParent(path);
    char full[128];
    char tmp[136];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    int n = std::snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    size_t written = std::fwrite(data, 1, len, f);
    if (written != len) {
        std::fclose(f);
        ::remove(tmp);
        return false;
    }
    std::fflush(f);
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
    std::fclose(f);

    if (::rename(tmp, full) != 0) {
        ::remove(tmp);
        return false;
    }
    return true;
}

int fsReadAt(const char* path, long offset, char* buf, size_t len) {
    if (!fsMounted_ || !buf) return -1;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return -1;
    FILE* f = std::fopen(full, "rb");
    if (!f) return -1;
    if (std::fseek(f, offset, SEEK_SET) != 0) { std::fclose(f); return -1; }
    const size_t n = std::fread(buf, 1, len, f);
    std::fclose(f);
    return static_cast<int>(n);   // 0 at EOF
}

long fsSize(const char* path) {
    if (!fsMounted_) return -1;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return -1;
    struct stat st;
    if (::stat(full, &st) != 0 || S_ISDIR(st.st_mode)) return -1;
    return static_cast<long>(st.st_size);
}

bool fsWriteStream(const char* path, FsWriteSrc src, void* user) {
    if (!fsMounted_ || !src) return false;
    makeParent(path);
    char full[128];
    char tmp[136];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    int n = std::snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    // Chunks straight through, so any file size fits a fixed buffer; an aborted source discards.
    char chunk[1024];
    bool ok = true, abort = false;
    for (;;) {
        const size_t got = src(chunk, sizeof(chunk), user, &abort);
        if (abort) { ok = false; break; }
        if (got == 0) break;                                    // clean end of stream
        if (std::fwrite(chunk, 1, got, f) != got) { ok = false; break; }
    }
    std::fflush(f);
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
    std::fclose(f);
    if (!ok || ::rename(tmp, full) != 0) { ::remove(tmp); return false; }
    return true;
}

void fsList(const char* dir, FsListCb cb, void* user) {
    if (!fsMounted_ || !cb) return;
    char full[128];
    if (!fsTranslate(dir, full, sizeof(full))) return;
    DIR* d = ::opendir(full);
    if (!d) return;
    struct dirent* ent;
    // Sized to hold full ("/littlefs/..." up to 128) + '/' + max 255-byte d_name + null.
    char childPath[400];
    struct stat st;
    while ((ent = ::readdir(d)) != nullptr) {
        std::snprintf(childPath, sizeof(childPath), "%s/%s", full, ent->d_name);
        const bool statOk = stat(childPath, &st) == 0;
        const bool isDir = statOk && S_ISDIR(st.st_mode);
        const uint32_t size = (statOk && !isDir) ? static_cast<uint32_t>(st.st_size) : 0;
        cb(ent->d_name, isDir, size, user);
    }
    ::closedir(d);
}

size_t filesystemUsed() {
    if (!fsMounted_) return 0;
    size_t total = 0, used = 0;
    if (!fsLabelInUse_) return 0;
    if (esp_littlefs_info(fsLabelInUse_, &total, &used) != ESP_OK) return 0;
    return used;
}

size_t filesystemTotal() {
    if (!fsMounted_) return 0;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(fsLabelInUse_, &total, &used) != ESP_OK) return 0;
    return total;
}

} // namespace mm::platform
