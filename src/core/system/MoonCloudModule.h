// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once


#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/module/MoonModule.h"
#include "core/util/JsonFetch.h"   // the worker an HTTPS post goes out on
#include "core/util/sha256.h"
#include "platform/platform.h"

namespace mm {

/// The container for everything MoonLight does with a server MoonModules runs.
/// @card MoonCloudModule.png
///
/// It holds no settings of its own. Each thing it does is a child with its own consent: @ref MoonStatsModule reports the install and @ref MoonTalkModule is a message board. A user who wants one has not agreed to the other.
///
/// @technical
///
/// ## The browser relay, on a board without HTTPS
///
/// A board with no TLS cannot post, so its one outbound call goes into a one-slot outbox, a hidden control the open page reads. The page posts it, then clears it by writing the request's `id` to the hidden `outbox sent`.
///
/// A board without PSRAM sends itself while a TLS session fits in internal RAM, and uses the page otherwise; one with PSRAM always sends itself. Two pages can post the same request, a rare double a usage count tolerates.
class MoonCloudModule : public MoonModule {
public:
    /// How a request leaves: the board itself, the open page, or the board while a TLS session fits and the page otherwise.
    enum class Route : uint8_t { Direct, Page, DirectOrPage };

    /// This board's route: the page without an HTTPS client, the board itself where TLS sits in PSRAM, and either without PSRAM.
    static Route boardRoute() {
        if (!platform::httpsAvailable()) return Route::Page;
        return platform::hasPsram ? Route::Direct : Route::DirectOrPage;
    }

    /// A board that may need the page gets its outbox at construction, so the relay's controls are declared with the rest.
    explicit MoonCloudModule(Route route = boardRoute())
        : route_(route),
          outbox_(route != Route::Direct ? static_cast<char*>(platform::alloc(kOutboxBytes)) : nullptr) {
        if (!outbox_) return;
        outbox_[0] = '\0';
        setDynamicBytes(kOutboxBytes);   // the card's memory readout counts it
    }
    /// Free the outbox, where there is one, and the replies held on the worker.
    ~MoonCloudModule() override {
        platform::free(outbox_);
        JsonFetch::instance().release(this);
    }

    /// Always runs: the children carry the real consent, so disabling the container is ambiguous.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// The relay's two hidden controls where there is an outbox, then the children's.
    void defineControls() override {
        controls_.clear();
        if (outbox_) {
            // Read-only, which also keeps it out of the saved settings: a request is transient.
            controls_.addText("outbox", outbox_, kOutboxBytes);
            controls_.setReadOnly(controls_.count() - 1, true);
            controls_.setHidden(controls_.count() - 1, true);
            controls_.addControl("outbox sent", acked_, 0, INT32_MAX);
            controls_.setHidden(controls_.count() - 1, true);
        }
        MoonModule::defineControls();
    }

    /// Number the next request after the last one acknowledged, which the settings restored, so a new request never matches it.
    void setup() override {
        lastId_ = acked_;
        MoonModule::setup();
    }

    /// Empty the outbox once the page has delivered the request waiting in it. Only the waiting request's id counts, so a late acknowledgement of an earlier one leaves a newer one waiting.
    void onControlChanged(const char* name) override {
        if (outbox_ && outbox_[0] && name && std::strcmp(name, "outbox sent") == 0 && acked_ == pendingId_) {
            outbox_[0] = 0;
            notifyValuesChanged();
        }
    }

    /// Whether a post can leave this board at all: over HTTPS, or through the page.
    bool canSend() const { return route_ != Route::Page || outbox_ != nullptr; }
    /// Whether every post waits for the page, the board having no HTTPS client.
    bool relaysThroughBrowser() const { return route_ == Route::Page; }
    /// Whether the last post went to the page rather than out of the board.
    bool lastViaPage() const { return lastViaPage_; }

    /// Whether a request is on its way, to the page or out of the board.
    bool waiting() MM_NONBLOCKING { settle(); return pending_ != 0; }
    /// Whether the request behind `ticket` is still on its way, a ticket reading the same on either route.
    bool waiting(uint32_t ticket) MM_NONBLOCKING { settle(); return ticket && ticket == pending_; }
    /// Whether the request behind `ticket` arrived: the page delivered it, or the server answered 2xx. An older ticket's outcome is gone, and reads as not arrived.
    bool delivered(uint32_t ticket) MM_NONBLOCKING { settle(); return ticket && ticket == settled_ && settledOk_; }

    /// The one outbound call, over HTTPS or into the outbox for the page (@xref{the-browser-relay-on-a-board-without-https}). A ticket for its outcome; 0 when it could not go or one is on its way.
    uint32_t post(const char* path, const char* body) {
        if (waiting()) return 0;
        char url[192];
        std::snprintf(url, sizeof(url), "https://%s%s", kHost, path);
        const bool direct = route_ == Route::Direct || (route_ == Route::DirectOrPage && platform::tlsRoom(url));
        lastViaPage_ = !direct;
        if (direct) reply_ = JsonFetch::instance().post(this, url, body);
        if (direct ? !reply_ : !queueForPage(path, body)) return 0;
        ticket_ = ticket_ + 1 ? ticket_ + 1 : 1;
        pending_ = ticket_;
        return pending_;
    }

private:
    /// Put the request in the outbox for the open page, numbered after the last one; false when there is no outbox or it does not fit.
    bool queueForPage(const char* path, const char* body) {
        if (!outbox_) return false;
        pendingId_ = lastId_ < INT32_MAX ? lastId_ + 1 : 1;
        lastId_ = pendingId_;
        const int n = std::snprintf(outbox_, kOutboxBytes, "{\"id\":%ld,\"path\":\"%s\",\"body\":%s}",
                                    static_cast<long>(pendingId_), path, body);
        if (n < 0 || static_cast<size_t>(n) >= kOutboxBytes) { outbox_[0] = 0; return false; }
        notifyValuesChanged();   // to the page on the next transport tick
        return true;
    }

    /// Settle the request on its way once it is done, keeping its outcome for its ticket. A read of fields alone: the loop takes the answers in, and the reply stays held for the next post.
    void settle() MM_NONBLOCKING {
        if (!pending_) return;
        if (lastViaPage_) {
            if (outbox_ && outbox_[0]) return;
            settledOk_ = true;   // the page empties the outbox once the server took it
        } else {
            const int status = JsonFetch::instance().outcome(this, reply_);
            if (status < 0) return;
            settledOk_ = status >= 200 && status < 300;
        }
        settled_ = pending_;
        pending_ = 0;
    }

    /// How a request leaves this board.
    Route route_ = Route::Direct;
    /// Whether the last post went to the page.
    bool lastViaPage_ = false;
    /// The stats host, compiled in rather than configurable, on this name because a custom domain's CA is absent.
    static constexpr const char* kHost = "mooncloud-stats.moonmodules.workers.dev";
    /// The outbox size for one request: a usage report is well under it, and a talk message is capped at 280 characters.
    static constexpr uint16_t kOutboxBytes = 2048;
    /// The request waiting for the open page to post it.
    char* outbox_ = nullptr;
    /// The id of the request the page delivered, which empties the outbox.
    int32_t acked_ = 0;
    int32_t pendingId_ = 0;    ///< the id of the request waiting in the outbox
    int32_t lastId_ = 0;       ///< the id given last
    int      reply_ = 0;        ///< the reply to the last HTTPS post, which the JSON client's worker sends so the loop never waits for a handshake; 0 when it could not go
    uint32_t ticket_ = 0;       ///< the last ticket given
    uint32_t pending_ = 0;      ///< the ticket of the request on its way, 0 when none is
    uint32_t settled_ = 0;      ///< the ticket of the last request that finished
    bool     settledOk_ = false;   ///< and whether it arrived
};


/// The installation id, a salted hash of the MAC: pseudonymous, and salted apart from the MQTT and Home Assistant ids.

/// Characters written by `installationId`, excluding the terminator.
inline constexpr size_t kInstallationIdChars = 32;

/// Changing this re-identifies every installation in the world exactly once, so it keeps the spelling it shipped with.
inline constexpr char kMoonCloudSalt[] = "projectMM/MoonStats/v1";

/// Write the installation id into `out`, which must hold one more character than the id.
inline void installationId(char* out) {
    if (!out) return;

    uint8_t mac[6] = {};
    platform::getMacAddress(mac);

    // Concatenation rather than HMAC: the salt is public either way, so it separates rather than keys.
    uint8_t buf[sizeof(kMoonCloudSalt) - 1 + sizeof(mac)];
    std::memcpy(buf, kMoonCloudSalt, sizeof(kMoonCloudSalt) - 1);
    std::memcpy(buf + sizeof(kMoonCloudSalt) - 1, mac, sizeof(mac));

    sha256Hex(buf, sizeof(buf), out, kInstallationIdChars / 2);
}

}  // namespace mm
