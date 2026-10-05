/// @defgroup platform_esp32_ota Over-the-air updates
/// Fetch firmware from a URL and write it to the next update partition.
///
/// The file owns its task shape privately; the rest of the layer reaches it through one declared symbol.
///
/// @moreinfo
///
/// ## Writing the wrong image is the unrecoverable direction
///
/// The two images sit side by side on the releases page and are one paste apart.
/// Writing the recovery image into the application slot leaves both partitions holding it, and every route out then resolves to the partition being run from.
/// The device answers and serves a page and cannot be recovered without a cable, which was found on the bench by installing exactly that.
///
/// So the check reads from the incoming image before a byte is committed, and a wrong file costs nothing.
/// It decides on ENOUGH bytes rather than on the first chunk: a producer may deliver fewer than the descriptor needs.
/// And identifying a short prefix reports no description for an image that has one, so the guard would pass and the wrong image would land.
///
/// ## Updating the recovery image itself
///
/// The application installs it, the mirror of it installing the application, and nothing else can.
/// A board cannot rewrite the partition it is executing from, so each slot is writable only while the other is running.
/// Without this path a device whose recovery image is broken needs a cable, which is the one failure that image exists to prevent, and a bench board hit exactly that.
///
/// The update interface cannot serve it, targeting only its own partition subtypes, so this writes raw flash and loses the validation that interface performs.
/// The checks below replace it and their ORDER is the safety property: everything that can reject an image runs from the first chunk, before a single byte is erased.
///
/// ## Short chunks are kept, not padded
///
/// The raw write needs a word-aligned length, which the update interface used to absorb.
/// So only whole words are written and the remainder is kept for the next read to extend.
/// Padding a short chunk looks right and is not: a chunk arrives short whenever the producer has less to give.
/// Which on a network path is most reads rather than only the last.
/// The padding then reaches flash, the offset advances by the unpadded count, and the next write lands back over it, corrupting the image at every short read.
/// The verification catches the result, but only after the slot is erased, which is the failure this design exists to avoid.
/// At the end the remainder is the image's last bytes, so it is padded to a word with what erased flash reads as; only there is padding correct.
///
/// ## The redirect response needs a bigger header buffer
///
/// The default is far too small for the host's redirect, whose policy header overflows it and fails the update before the download starts.
/// Raising both directions covers the longest headers with room to spare, at a few kilobytes of heap freed when the task exits.
///
/// ## Progress rides the status string
///
/// Both writers report the counts in the same shape, so the interface reads progress identically whichever image is installed.
/// They used to reach a control that no longer exists, which showed an indeterminate sweep that never resolved on a device working perfectly.
/// The padded tail is excluded: it is flash the image does not occupy, and counting it would report more written than was sent.
///
/// ## Why the install runs on its own task
///
/// The HTTP request answers 202 at once, exactly as the application's URL install does.
/// While a request is open the browser cannot poll for progress, so a synchronous install would leave it blind for the whole write.
///
/// ## The application answers before it reboots
///
/// The image is committed and the boot pointer flipped, so the caller returns and sends its HTTP 200 BEFORE the restart, the same sequence /api/reboot follows.
/// That is what lets a browser see a clean result rather than a dropped socket.
///
/// ## Reading a header straight out of a partition
///
/// The image's length sits in its header, a plain partition read needing no flash mapping.
///
/// ## Reporting on the recovery image
///
/// Which recovery image a device carries is read from its descriptor at a fixed offset, costing one flash read and no reboot.
/// The recovery image answers the same question about itself elsewhere, which is a different question rather than a second copy.
/// That reports what it is executing, and this reports an image it is not.
/// Both carry the same version string, which is what lets a caller compare them by equality.
/// Its SIZE comes from the slot rather than the image.
/// The metadata call reads through a mapping arranged for the running partition, and errored for this one, leaving the row absent.
/// What a user wants from the row is whether the slot has room, and reading the descriptor already proves an image is there.
/// The figure counts the segments only, so trailing padding, checksum and hash are excluded and it reads a few dozen bytes under the file on disk.
/// That is deliberate: the figure answers how full the slot is, and reproducing the bootloader's padding rules would be a second copy to keep in step for no gain.
///
/// ## Pointing the bootloader at the recovery image
///
/// Setting the boot partition to it erases the selection data rather than writing a sequence number.
/// Which is what makes a power cut land back in recovery rather than in a half-written application.
///
/// ## A URL pull reuses the upload writer
///
/// The update interface cannot serve a recovery image from a URL, targeting only its own subtypes and picking the partition itself.
/// So the download is driven by hand through the same producer callback: one writer, one set of checks, two sources.
///
/// ## The single-slot guard
///
/// The next-partition call falls back to the first slot it finds, so on a one-slot table it hands back the partition being executed, and erasing that bricks the device mid-write.
/// The interface refuses it too, but failing early says why and names the fix.
///
/// ## The update buffer is heap and owned
///
/// Heap rather than static or stack: too large for a task frame, and a static one held internal memory from boot for nothing.
/// An update is the one moment when spare memory is least scarce, so allocating here gives it back to the network stack for the rest of uptime.
/// Owned rather than raw, since six exit paths would each leak it.
///
/// ## One bounded status writer
///
/// Three identical local copies each put the format behind a template, which defeats the compiler's format check.
/// A plain variadic function takes the format attribute instead, so a non-literal format is a build error rather than a scanner note.

#include "platform/platform.h"

#include "core/util/FirmwareImage.h"  // identify, the image names and the stall limit: vetting what arrives

#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"   // esp_partition_find_first/erase_range/write: MoonBase
#include "esp_image_format.h" // esp_image_verify + the image/segment headers
#include "sdkconfig.h"     // CONFIG_IDF_FIRMWARE_CHIP_ID: which chip THIS build is for
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_heap_caps.h"   // heap_caps_malloc/free — the upload chunk buffer
#include "esp_log.h"
#include "nvs.h"           // moonbaseStageInstallUrl: the URL handoff to MoonBase

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>        // unique_ptr — frees the upload buffer on every exit path
#include <new>           // std::nothrow for the OtaTaskParams alloc below


namespace mm::platform {

// One upload chunk. 4 KB matches the flash page granularity esp_ota_write prefers and is the size the HTTP path already streams in.
constexpr size_t kOtaChunkBytes = 4096;

// The factory partition, which holds MoonBase, or null on the dual-OTA tables. One lookup for every caller below: the same find_first was written out four times as this file grew.
const esp_partition_t* moonBasePartition() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}

namespace {

// Heap-allocated task parameters. Task owns this and frees it on exit.
struct OtaTaskParams {
    char url[512];
    char fallbackUrl[512] = {};   // empty where the caller named none
    char* statusBuf;
    size_t statusBufLen;
    uint32_t* bytesReadOut;   // current bytes downloaded
    uint32_t* bytesTotalOut;  // image size; 0 until esp_https_ota reports it
};

// One bounded status writer for the file: @xref{one-bounded-status-writer|why a plain variadic beats a template}.
__attribute__((format(printf, 3, 4)))
void statusf(char* buf, size_t len, const char* fmt, ...) {
    if (!buf || len == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, len, fmt, args);
    va_end(args);
}

// The same format check statusf carries, so a non-literal format is a build error rather than a runtime surprise.
__attribute__((format(printf, 2, 3)))
void otaSetStatus(OtaTaskParams* p, const char* fmt, ...) {
    if (!p->statusBuf || p->statusBufLen == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(p->statusBuf, p->statusBufLen, fmt, args);
    va_end(args);
}

void otaTask(void* arg) {
    auto* p = static_cast<OtaTaskParams*>(arg);

    otaSetStatus(p, "downloading");
    *p->bytesReadOut = 0;
    *p->bytesTotalOut = 0;   // unknown until esp_https_ota reports it

    // The bundled trust anchors, the same mechanism a browser uses, with no certificate baked in. Attached unconditionally: a plain local URL leaves them unused while still satisfying the begin call.
    esp_http_client_config_t http_config = {};
    http_config.url = p->url;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = 10000;
    // GitHub release-asset URLs 302-redirect to objects.githubusercontent.com. Default redirect handling is off in esp_http_client; force-follow.
    http_config.disable_auto_redirect = false;
    http_config.max_redirection_count = 10;
    // A bigger header buffer: @xref{the-redirect-response-needs-a-bigger-header-buffer|what overflows the default}.
    http_config.buffer_size = 4096;
    http_config.buffer_size_tx = 4096;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;
    // Performs partial-image-write + commit + boot-pointer flip internally.

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    // On a failed BEGIN only: once bytes flow the partition holds them, and a second address would restart that.
    if (err != ESP_OK && p->fallbackUrl[0]) {
        otaSetStatus(p, "retrying the other address");
        http_config.url = p->fallbackUrl;
        handle = nullptr;
        err = esp_https_ota_begin(&ota_config, &handle);
    }
    if (err != ESP_OK) {
        // esp_https_ota_begin collapses ~6 distinct failures (DNS, TLS, HTTP, partition init, header-buffer overflow) into one ESP_FAIL, so the only useful detail is in the IDF log on the serial console. We surface the IDF error name plus a pointer to the log.
        otaSetStatus(p, "error: ota begin %s (see serial log)",
                     esp_err_to_name(err));
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    // Refuse a recovery image here: @xref{writing-the-wrong-image-is-the-unrecoverable-direction|why this direction is the worse one}.
    esp_app_desc_t incoming = {};
    const bool haveDesc = esp_https_ota_get_img_desc(handle, &incoming) == ESP_OK;
    if (haveDesc && firmware::isMoonBaseImage(incoming.project_name, sizeof(incoming.project_name))) {
        otaSetStatus(p, "error: that is a MoonBase image, not an app");
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }
    // And refuse an image that is not this project: a URL can name another repository's release, and the descriptor is what says whose firmware arrived.
    if (haveDesc && !firmware::isAppImage(incoming.project_name, sizeof(incoming.project_name))) {
        otaSetStatus(p, "error: that image is %.*s, not this project",
                     static_cast<int>(sizeof(incoming.project_name)), incoming.project_name);
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    if (total > 0) {
        // Publish the real total so the UI can render "X KB / Y KB". FirmwareUpdateModule's tick1s() rebuildControls picks this up on the next 1 Hz poll (re-binds the progress descriptor with the new total snapshot).
        *p->bytesTotalOut = static_cast<uint32_t>(total);
    }
    otaSetStatus(p, "flashing");

    int lastGot = -1;
    TickType_t movedAt = xTaskGetTickCount();
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(handle);
        if (got >= 0) *p->bytesReadOut = static_cast<uint32_t>(got);
        // Give up on a connection that stopped delivering, which ESP-IDF reports as still in progress: FirmwareImage.h names why.
        if (got != lastGot) {
            lastGot = got;
            movedAt = xTaskGetTickCount();
        } else if (pdTICKS_TO_MS(xTaskGetTickCount() - movedAt) > firmware::kDownloadStallMs) {
            otaSetStatus(p, "error: the download stalled");
            esp_https_ota_abort(handle);
            delete p;
            vTaskDelete(nullptr);
            return;
        }
        // The counts ride the status: @xref{progress-rides-the-status-string|why, and what the padding is excluded from}.
        if (total > 0) {
            otaSetStatus(p, "flashing: %u of %u bytes",
                         static_cast<unsigned>(got > 0 ? got : 0), static_cast<unsigned>(total));
        }
    }
    if (err != ESP_OK) {
        otaSetStatus(p, "error: ota perform %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        otaSetStatus(p, "error: incomplete download");
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        // After finish, abort isn't valid, handle is consumed. Surface and exit.
        otaSetStatus(p, "error: ota finish %s", esp_err_to_name(err));
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    // Final byte count match, pull from the OTA handle one last time so the UI's last frame before reboot shows a clean "Y KB / Y KB".
    if (*p->bytesTotalOut > 0) *p->bytesReadOut = *p->bytesTotalOut;
    otaSetStatus(p, "rebooting");
    delete p;
    // 600 ms delay gives the HTTP response time to make it back to the browser before the device drops the socket on restart.
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

}  // anonymous namespace

bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut,
                       const char* fallbackUrl) {
    if (!url || !statusBuf || statusBufLen == 0 || !bytesReadOut || !bytesTotalOut) {
        return false;
    }

    // Reject oversize URLs explicitly rather than silently truncating with strncpy, a truncated URL almost always 404s or fetches the wrong file, with no clue in the status surface.
    size_t urlLen = std::strlen(url);
    constexpr size_t kUrlMax = sizeof(OtaTaskParams::url) - 1;
    if (urlLen > kUrlMax) {
        std::snprintf(statusBuf, statusBufLen,
                      "error: url too long (%zu > %zu)", urlLen, kUrlMax);
        return false;
    }

    // std::nothrow so OOM doesn't abort the process. Status string carries the failure back to the route, which returns 500 to the browser.
    auto* p = new (std::nothrow) OtaTaskParams{};
    if (!p) {
        std::snprintf(statusBuf, statusBufLen, "error: out of memory");
        return false;
    }
    std::memcpy(p->url, url, urlLen + 1);   // includes NUL; size already verified
    // An over-long fallback is dropped rather than refused: the primary URL is still worth trying.
    if (fallbackUrl && fallbackUrl[0]) {
        const size_t fLen = std::strlen(fallbackUrl);
        if (fLen <= sizeof(p->fallbackUrl) - 1) std::memcpy(p->fallbackUrl, fallbackUrl, fLen + 1);
    }
    p->statusBuf = statusBuf;
    p->statusBufLen = statusBufLen;
    p->bytesReadOut = bytesReadOut;
    p->bytesTotalOut = bytesTotalOut;

    // 12 KB stack matches v1's working number (TLS handshake + HTTPS body buffering inside esp_https_ota). Priority 5 = above idle, below FreeRTOS critical drivers.
    BaseType_t ok = xTaskCreate(&otaTask, "urlOta", 12288, p, 5, nullptr);
    if (ok != pdPASS) {
        otaSetStatus(p, "error: task create failed");
        delete p;
        return false;
    }
    return true;
}


bool otaWriteStream(FsWriteSrc src, void* user, size_t contentLen,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    if (!src || !statusBuf || statusBufLen == 0 || !bytesReadOut) return false;
    // A macro, not a lambda: the format reaches statusf as a literal. Through a lambda it arrives as a runtime pointer, which defeats the format check.
    #define setStatus(...) statusf(statusBuf, statusBufLen, __VA_ARGS__)

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { setStatus("error: no OTA partition"); return false; }

    // The single-slot guard: @xref{the-single-slot-guard|what the fallback would erase}.
    if (part == esp_ota_get_running_partition()) {
        setStatus("error: one app slot, reboot to MoonBase first");
        return false;
    }
    // SIZE GUARD. Without it an oversized image fails partway through esp_ota_write, leaving the target slot half-written and the user staring at a generic write error. Content-Length is advisory, so this only fires when the caller knows the size.
    if (contentLen && contentLen > part->size) {
        setStatus("error: image too large (%u > %u)",
                  static_cast<unsigned>(contentLen), static_cast<unsigned>(part->size));
        return false;
    }

    setStatus("flashing");
    esp_ota_handle_t handle = 0;
    // Each sector is erased as the write reaches it: OTA_SIZE_UNKNOWN would erase the whole slot here in one call, seconds the calling task cannot feed its watchdog through.
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) { setStatus("error: ota begin %s", esp_err_to_name(err)); return false; }

    // The body, chunk by chunk through the writer's own producer callback: @xref{the-update-buffer-is-heap-and-owned|why heap, and why owned}.
    const std::unique_ptr<uint8_t, decltype(&heap_caps_free)> owned(
        static_cast<uint8_t*>(heap_caps_malloc(kOtaChunkBytes, MALLOC_CAP_8BIT)), &heap_caps_free);
    uint8_t* const buf = owned.get();
    if (!buf) {
        setStatus("error: out of memory for the upload buffer");
        esp_ota_abort(handle);
        return false;
    }
    uint32_t written = 0;
    bool vetted = false;
    for (;;) {
        bool abort = false;
        const size_t n = src(reinterpret_cast<char*>(buf), kOtaChunkBytes, user, &abort);
        if (abort) {
            setStatus("error: upload aborted");
            esp_ota_abort(handle);
            return false;
        }
        if (n == 0) break;   // clean EOF — whole body delivered
        // Refuse a recovery image, decided on enough bytes rather than on the first chunk: @xref{writing-the-wrong-image-is-the-unrecoverable-direction|why a short prefix would let it through}.
        if (!vetted) {
            if (written + n < firmware::kIdentifyBytes) {
                // A rejected image must leave no bytes, so a body ending before it can be identified is refused rather than kept.
                if (contentLen && contentLen < firmware::kIdentifyBytes) {
                    setStatus("error: too short to be a firmware image");
                    esp_ota_abort(handle);
                    return false;
                }
            }
            const auto info = firmware::identify(buf, n);
            if (info.valid && !info.described && n < firmware::kIdentifyBytes) {
                setStatus("error: could not identify the image");
                esp_ota_abort(handle);
                return false;
            }
            vetted = true;
            if (info.described && firmware::isMoonBaseImage(info.project)) {
                setStatus("error: that is a MoonBase image, not an app");
                esp_ota_abort(handle);
                return false;
            }
            // And whose firmware this is, the same question the URL path asks, before the first write rather than after the slot is spent.
            if (!info.described || !firmware::isAppImage(info.project)) {
                setStatus("error: that image is not this project");
                esp_ota_abort(handle);
                return false;
            }
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            setStatus("error: ota write %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            return false;
        }
        written += static_cast<uint32_t>(n);
        *bytesReadOut = written;
        taskWdtReset();   // the upload runs on the render task, which a 2 MB stream would otherwise starve past its watchdog
    }
    // Guard a truncated upload: if the client sent fewer bytes than Content-Length, the image is incomplete, don't commit a half-image. (contentLen 0 = unknown; skip the check then.)
    if (contentLen && written < contentLen) {
        setStatus("error: incomplete upload (%u/%u)",
                  static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
        esp_ota_abort(handle);
        return false;
    }

    err = esp_ota_end(handle);   // validates the image (magic/checksum); consumes the handle
    if (err != ESP_OK) { setStatus("error: ota end %s", esp_err_to_name(err)); return false; }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) { setStatus("error: set boot %s", esp_err_to_name(err)); return false; }

    setStatus("rebooting");
    // Committed and flipped: @xref{the-application-answers-before-it-reboots|why the caller returns first}.
    return true;
    #undef setStatus
}

// Updating the recovery image itself: @xref{updating-the-recovery-image-itself|why only the application can}.

// Does this first chunk begin a MoonBase image for THIS chip? The rules live in core/FirmwareImage.h so a host test can drive them.
bool moonBaseImageRejected(const uint8_t* buf, size_t n, char* why, size_t whyLen) {
    const auto info = firmware::identify(buf, n);
    const char* reason = firmware::moonBaseRejection(
        info, static_cast<firmware::ChipId>(CONFIG_IDF_FIRMWARE_CHIP_ID));
    if (!reason) return false;
    std::snprintf(why, whyLen, "error: %s", reason);
    return true;
}

bool otaWriteMoonBase(FsWriteSrc src, void* user, size_t contentLen,
                      char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    if (!src || !statusBuf || statusBufLen == 0 || !bytesReadOut) return false;
    // A macro, not a lambda: the format reaches statusf as a literal. Through a lambda it arrives as a runtime pointer, which defeats the format check.
    #define setStatus(...) statusf(statusBuf, statusBufLen, __VA_ARGS__)

    const esp_partition_t* part = moonBasePartition();
    if (!part) { setStatus("error: no MoonBase on this device"); return false; }
    // The inverse of otaWriteStream's single-slot guard, and the reason this path is safe at all: the app runs from ota_0, so factory is never what we are executing. Running from MoonBase means the app slot is the writable one, and this is the wrong call.
    if (part == esp_ota_get_running_partition()) {
        setStatus("error: running from MoonBase, boot the app first");
        return false;
    }
    if (contentLen && contentLen > part->size) {
        setStatus("error: image too large (%u > %u)",
                  static_cast<unsigned>(contentLen), static_cast<unsigned>(part->size));
        return false;
    }

    const std::unique_ptr<uint8_t, decltype(&heap_caps_free)> owned(
        static_cast<uint8_t*>(heap_caps_malloc(kOtaChunkBytes, MALLOC_CAP_8BIT)), &heap_caps_free);
    uint8_t* const buf = owned.get();
    if (!buf) { setStatus("error: out of memory for the update buffer"); return false; }

    setStatus("checking");
    // FIRST CHUNK BEFORE ANY ERASE. Everything that can reject the image is decided here, while the existing MoonBase is still intact and the worst case is a status line.
    bool abort = false;
    size_t first = src(reinterpret_cast<char*>(buf), kOtaChunkBytes, user, &abort);
    if (abort || first == 0) { setStatus("error: no image received"); return false; }
    if (moonBaseImageRejected(buf, first, statusBuf, statusBufLen)) return false;

    // PAST THIS LINE THE DEVICE HAS NO RECOVERY IMAGE until the write completes: @xref{updating-the-recovery-image-itself|why one pass}.
    setStatus("erasing");
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) { setStatus("error: erase %s", esp_err_to_name(err)); return false; }

    setStatus("writing MoonBase");
    uint32_t written = 0;     // bytes committed to flash
    size_t   held    = first; // bytes sitting in buf, not yet written
    bool     eof     = false;
    for (;;) {
        // Write whole words only and keep the remainder for the next read: @xref{short-chunks-are-kept-not-padded|why padding one corrupts the image}.
        const size_t whole = eof ? ((held + 3u) & ~size_t{3}) : (held & ~size_t{3});
        if (whole) {
            if (eof && whole > held) std::memset(buf + held, 0xFF, whole - held);
            // Compared on the IMAGE bytes: at EOF `whole` rounds past the end, and refusing a slot-filling image for its own padding would refuse something that fits.
            if (written + (eof ? held : whole) > part->size) {
                setStatus("error: image overruns the slot");
                return false;
            }
            err = esp_partition_write(part, written, buf, whole);
            if (err != ESP_OK) { setStatus("error: write %s", esp_err_to_name(err)); return false; }
            // The IMAGE grew by what it held, never by the padding.
            written += static_cast<uint32_t>(eof ? held : whole);
            *bytesReadOut = written;
            // The counts ride the STATUS, the way MoonBase's own page reports them.
            if (contentLen) {
                setStatus("writing MoonBase: %u of %u bytes",
                          static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
            }
            held -= eof ? held : whole;
            taskWdtReset();   // the render task carries this stream too
        }
        if (eof) break;

        // Keep the unwritten remainder at the front, then read after it.
        if (held) std::memmove(buf, buf + (whole ? whole : 0), held);
        abort = false;
        const size_t got = src(reinterpret_cast<char*>(buf) + held, kOtaChunkBytes - held,
                               user, &abort);
        if (abort) { setStatus("error: transfer aborted, MoonBase is incomplete"); return false; }
        if (got == 0) eof = true;      // one more pass, to flush what is held
        held += got;
    }
    if (contentLen && written < contentLen) {
        setStatus("error: incomplete transfer (%u/%u), MoonBase is incomplete",
                  static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
        return false;
    }

    // What esp_ota_end did on the app path: checksum the image actually on flash. A failure here is the serious one, since the slot no longer holds a working MoonBase, so it says so plainly rather than reporting a generic write error.
    esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    esp_image_metadata_t meta = {};
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) != ESP_OK) {
        setStatus("error: MoonBase did not verify, retry before rebooting");
        return false;
    }
    // No reboot and no boot-partition change: the app keeps running, and the new MoonBase is what the device falls back to from now on.
    setStatus("idle");
    return true;
    #undef setStatus
}

// Pull a recovery image from a URL into the same writer the upload path uses: @xref{a-url-pull-reuses-the-upload-writer|why by hand}.
struct UrlPull {
    esp_http_client_handle_t client;
    bool failed;
};

size_t urlPullChunk(char* out, size_t cap, void* user, bool* abort) {
    auto* u = static_cast<UrlPull*>(user);
    const int n = esp_http_client_read(u->client, out, static_cast<int>(cap));
    if (n < 0) { u->failed = true; *abort = true; return 0; }
    return static_cast<size_t>(n);   // 0 is a clean end of body
}

bool moonBaseFetchUrlSync(const char* url, char* statusBuf, size_t statusBufLen,
                          uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    // A macro, not a lambda: the format reaches statusf as a literal. Through a lambda it arrives as a runtime pointer, which defeats the format check.
    #define setStatus(...) statusf(statusBuf, statusBufLen, __VA_ARGS__)
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    // Same TLS and redirect handling the app's OTA fetch needs, and for the same reasons: a release asset 302s to objects.githubusercontent.com, whose headers overflow the 512-byte default buffer.
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 10;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { setStatus("error: cannot reach that URL"); return false; }
    // Opened by hand, the client follows no redirect itself, and a release asset always answers 302: set the next location, drain the body and open again, the loop esp_https_ota runs.
    int64_t len = 0;
    int status = 0;
    for (int hop = 0; hop <= cfg.max_redirection_count; hop++) {
        const esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            setStatus("error: cannot start the download (%s)", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return false;
        }
        len = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status < 300 || status >= 400 || esp_http_client_set_redirection(client) != ESP_OK) break;
        char drain[256];
        while (esp_http_client_read(client, drain, sizeof(drain)) > 0) {}
    }
    if (status != 200) {
        // A 404 page is a valid HTTP response carrying HTML, which the image checks would catch anyway; failing here says the useful thing instead of "not a firmware image".
        setStatus("error: the server answered %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    if (bytesTotalOut) *bytesTotalOut = len > 0 ? static_cast<uint32_t>(len) : 0;

    UrlPull pull = { client, false };
    const bool ok = otaWriteMoonBase(&urlPullChunk, &pull, len > 0 ? static_cast<size_t>(len) : 0,
                                     statusBuf, statusBufLen, bytesReadOut);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok && pull.failed) setStatus("error: the download was interrupted");
    return ok;
    #undef setStatus
}

// Its own task, so the HTTP request answers 202 at once: @xref{why-the-install-runs-on-its-own-task|what a synchronous install costs}.
void moonBaseUrlTask(void* arg) {
    auto* p = static_cast<OtaTaskParams*>(arg);
    moonBaseFetchUrlSync(p->url, p->statusBuf, p->statusBufLen, p->bytesReadOut, p->bytesTotalOut);
    // No reboot on either outcome: the app is running from the other partition and is untouched. On failure the status already says what went wrong, and it stays there for the UI to read.
    delete p;
    vTaskDelete(nullptr);
}

bool otaFetchMoonBaseUrl(const char* url, char* statusBuf, size_t statusBufLen,
                         uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (!url || !statusBuf || statusBufLen == 0 || !bytesReadOut || !bytesTotalOut) return false;
    const size_t urlLen = std::strlen(url);
    auto* p = new (std::nothrow) OtaTaskParams{};
    if (!p) {
        std::snprintf(statusBuf, statusBufLen, "error: out of memory");
        return false;
    }
    if (urlLen >= sizeof(p->url)) {
        std::snprintf(statusBuf, statusBufLen, "error: url too long");
        delete p;
        return false;
    }
    std::memcpy(p->url, url, urlLen + 1);
    p->statusBuf = statusBuf;
    p->statusBufLen = statusBufLen;
    p->bytesReadOut = bytesReadOut;
    p->bytesTotalOut = bytesTotalOut;
    *bytesReadOut = 0;
    *bytesTotalOut = 0;
    std::snprintf(statusBuf, statusBufLen, "starting");
    // 8 KB against the app OTA's 12: no esp_https_ota state machine here, just an http client feeding a writer. Priority 5 and the same core assignment as urlOta.
    if (xTaskCreate(&moonBaseUrlTask, "mbOta", 8192, p, 5, nullptr) != pdPASS) {
        std::snprintf(statusBuf, statusBufLen, "error: task create failed");
        delete p;
        return false;
    }
    return true;
}


// Does this device's partition table carry a factory app? True on the MoonBase layout, false on the dual-OTA tables. The caller uses it to decide whether an update needs the reboot-into-MoonBase hop, and the UI to say which kind of device this is.
bool otaHasMoonBase() {
    return moonBasePartition() != nullptr;
}

// Which recovery image this device carries: @xref{reporting-on-the-recovery-image|why it is read here rather than asked of it}.
bool otaMoonBaseVersion(char* out, size_t len) {
    if (!out || len == 0) return false;
    out[0] = 0;
    const esp_partition_t* part = moonBasePartition();
    if (!part) return false;
    esp_app_desc_t desc = {};
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) return false;
    std::snprintf(out, len, "%.*s", static_cast<int>(sizeof(desc.version)), desc.version);
    return out[0] != 0;
}

// When MoonBase was built, from the same descriptor. The UI shows the app's build stamp beside its version, and shows the same pair for MoonBase, so this is the other half of that.
bool otaMoonBaseBuild(char* out, size_t len) {
    if (!out || len == 0) return false;
    out[0] = 0;
    const esp_partition_t* part = moonBasePartition();
    if (!part) return false;
    esp_app_desc_t desc = {};
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) return false;
    std::snprintf(out, len, "%.*s %.*s",
                  static_cast<int>(sizeof(desc.date)), desc.date,
                  static_cast<int>(sizeof(desc.time)), desc.time);
    return out[0] != 0;
}

// The recovery SLOT's size, for the interface to show beside the application's figure.
bool otaMoonBaseSize(uint32_t* used, uint32_t* total) {
    const esp_partition_t* part = moonBasePartition();
    if (!part) return false;
    if (total) *total = part->size;
    if (used) {
        // The length comes straight out of the header: @xref{reading-a-header-straight-out-of-a-partition|why no mapping is needed}.
        esp_image_header_t hdr = {};
        *used = 0;
        if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) == ESP_OK &&
            hdr.magic == ESP_IMAGE_HEADER_MAGIC) {
            // Walk the segment table to the end of the last segment: that is the image length.
            uint32_t off = sizeof(esp_image_header_t);
            for (uint8_t i = 0; i < hdr.segment_count && off < part->size; i++) {
                esp_image_segment_header_t seg = {};
                if (esp_partition_read(part, off, &seg, sizeof(seg)) != ESP_OK) { off = 0; break; }
                if (seg.data_len > part->size) { off = 0; break; }   // a corrupt length
                off += sizeof(seg) + seg.data_len;
            }
            // The segments only: @xref{reporting-on-the-recovery-image|why padding is excluded}.
            if (off) *used = off < part->size ? off : part->size;
        }
    }
    return true;
}

// Point the bootloader at the recovery image, false where the table has no such partition: @xref{pointing-the-bootloader-at-the-recovery-image|what a power cut then does}.
bool otaBootMoonBase() {
    const esp_partition_t* part = moonBasePartition();
    if (!part) return false;
    return esp_ota_set_boot_partition(part) == ESP_OK;
}

// Is the device currently executing FROM MoonBase?
bool otaRunningMoonBase() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    return run && run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
}

void moonbaseClearStagedUrl() {
    nvs_handle_t h;
    if (nvs_open("moonbase", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "url");
    nvs_erase_key(h, "tries");   // MoonBase's count of installs the URL started
    nvs_commit(h);
    nvs_close(h);
}

// Stage the install URL in NVS for MoonBase to consume on its next boot (see platform.h).
bool moonbaseStageInstallUrl(const char* url) {
    if (!url || !url[0]) return false;
    nvs_handle_t h;
    if (nvs_open("moonbase", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_erase_key(h, "tries");   // a new URL starts with every try MoonBase allows
    const bool ok = nvs_set_str(h, "url", url) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}


bool httpsAvailable() MM_NONBLOCKING {
    return true;   // the TLS stack is linked for OTA, so an ESP32 build can always try
}

bool httpsPost(const char* url, const char* body, uint32_t timeoutMs) {
    if (!url || !*url) return false;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = static_cast<int>(timeoutMs);
    // The SAME trust-anchor bundle the OTA path uses: IDF's built-in root certificates, so this verifies a public server without us shipping or maintaining a CA store.
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body ? body : "", body ? std::strlen(body) : 0);

    const esp_err_t err = esp_http_client_perform(client);
    const int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    return err == ESP_OK && status >= 200 && status < 300;
}

} // namespace mm::platform
