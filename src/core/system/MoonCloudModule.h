// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once


#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/module/MoonModule.h"
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
/// A report goes out whenever someone has the page open, enough for a usage count. Two pages can both post the waiting request, a rare double the count tolerates. A board with HTTPS has neither control.
class MoonCloudModule : public MoonModule {
public:
    /// A board without HTTPS gets its outbox at construction, so the relay's controls are declared with the rest.
    explicit MoonCloudModule(bool relay = !platform::httpsAvailable())
        : outbox_(relay ? static_cast<char*>(platform::alloc(kOutboxBytes)) : nullptr) {
        if (!outbox_) return;
        outbox_[0] = '\0';
        setDynamicBytes(kOutboxBytes);   // the card's memory readout counts it
    }
    /// Free the outbox, where there is one.
    ~MoonCloudModule() override { platform::free(outbox_); }

    /// Always runs: the children carry the real consent, so disabling the container is ambiguous.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// How long an outbound call may take before it is abandoned.
    static constexpr uint32_t kTimeoutMs = 4000;

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
    bool canSend() const { return platform::httpsAvailable() || outbox_ != nullptr; }
    /// Whether a post waits for the page rather than leaving at once.
    bool relaysThroughBrowser() const { return outbox_ != nullptr; }

    /// Whether a request still waits for the page to deliver it.
    bool outboxWaiting() const { return outbox_ && outbox_[0]; }

    /// The one outbound call, over HTTPS or into the outbox for the page (@xref{the-browser-relay-on-a-board-without-https}); false when it could not be handed off.
    bool post(const char* path, const char* body) {
        if (outbox_) {
            if (outbox_[0]) return false;
            pendingId_ = lastId_ < INT32_MAX ? lastId_ + 1 : 1;
            lastId_ = pendingId_;
            const int n = std::snprintf(outbox_, kOutboxBytes, "{\"id\":%ld,\"path\":\"%s\",\"body\":%s}",
                                        static_cast<long>(pendingId_), path, body);
            if (n < 0 || static_cast<size_t>(n) >= kOutboxBytes) { outbox_[0] = 0; return false; }
            notifyValuesChanged();   // to the page on the next transport tick
            return true;
        }
        char url[192];
        std::snprintf(url, sizeof(url), "https://%s%s", kHost, path);
        return platform::httpsPost(url, body, kTimeoutMs);
    }

private:
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
