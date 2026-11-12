#pragma once

/// The hardware-free half of a WiFi co-processor's status line: the link first, then the firmware version the chip reports over it.
///
/// @technical
///
/// ## The version query runs twice once the link is up, then settles
///
/// The P4's radio runs on a companion chip, and the query asks what firmware version it reported over the link. It is a blocking call over that link, and the module asks from the one-second tick, which runs inline on the render thread.
///
/// Measured on a board with the radio live: the call times out after about a second, every second, forever, so the module showed over a million microseconds per tick at zero frames.
///
/// The link works while this one call stays unanswered, so retrying does not help. Two attempts, since each unanswered one is a second of stutter and one retry still catches a companion mid-handshake.
///
/// They count only once the link is up: before the handshake the call fails at once, and attempts spent there left a working link reading as no reply.
///
/// After that the line keeps what it learned, since the version cannot change while the host runs.

#include "platform/nonblocking.h"

#include <cstdint>
#include <cstdio>

namespace mm::platform {

/// What the line reads while the link was not started, which a card compares to offer starting it.
inline constexpr const char* kLinkNotStarted = "link not started";

/// The link to the co-processor: unstarted while another interface carries the device, started and not up, or up.
enum class CoprocessorLink : uint8_t { NotStarted, Down, Up };

/// A firmware version as the co-processor reports it; `answered` is false when the query went unanswered.
struct CoprocessorVersion {
    bool answered = false;   ///< whether the query returned at all
    unsigned major = 0;      ///< the version's first part
    unsigned minor = 0;      ///< the version's second part
    unsigned patch = 0;      ///< the version's third part
};

/// The status line for a radio on a separate chip, held in one buffer, so a control bound to it follows every update; how often it asks: @xref{the-version-query-runs-twice-once-the-link-is-up-then-settles}.
class CoprocessorStatus {
public:
    /// Refresh from the link state, asking `query` for the version while attempts remain, and return the line.
    template <class Query>
    const char* update(CoprocessorLink link, Query query) {
        if (link != CoprocessorLink::Up) {
            std::snprintf(line_, sizeof(line_), "%s", link == CoprocessorLink::NotStarted ? kLinkNotStarted : "link down");
            return line_;
        }
        if (attemptsLeft_ > 0) {
            const CoprocessorVersion v = query();
            if (v.answered && (v.major || v.minor || v.patch)) {
                attemptsLeft_ = 0;
                reported_ = v;
                std::snprintf(version_, sizeof(version_), "C6 fw %u.%u.%u", v.major, v.minor, v.patch);
            } else if (--attemptsLeft_ == 0) {
                // The link is up and carries the radio, so an unanswered query is firmware that predates it, or answers 0.0.0.
                std::snprintf(version_, sizeof(version_), "C6 too old to report");
            }
        }
        std::snprintf(line_, sizeof(line_), "%s", version_);
        return line_;
    }

    /// The line as the last update left it.
    const char* line() const MM_NONBLOCKING { return line_; }

    /// Whether the settled answer differs from a host at `major`.`minor`: a version that differs, or none at all; false while the query is still to settle.
    bool outdated(unsigned major, unsigned minor) const MM_NONBLOCKING {
        if (attemptsLeft_ > 0) return false;
        return !reported_.answered || reported_.major != major || reported_.minor != minor;
    }

    /// The version the chip reported, `answered` false until it has.
    const CoprocessorVersion& reported() const { return reported_; }

private:
    CoprocessorVersion reported_;       ///< what the chip answered, once it has
    char line_[24] = "link not started";   ///< what a control shows
    char version_[24] = "querying…";    ///< the query's outcome, kept once settled
    uint8_t attemptsLeft_ = 2;          ///< queries left before the line settles
};

}  // namespace mm::platform
