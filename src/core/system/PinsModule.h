#pragma once


#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"   // instance()->moduleCount()/module(i): the roots of the tree to walk
#include "core/util/JsonSink.h"    // writeListRow emits its row as JSON into the sink
#include "core/util/Sort.h"        // insertionSort: order the map by GPIO (Device-Manager keying)
#include "platform/platform.h"  // gpioCapability: is a claimed pin a strap / input-only / reserved?
#include "core/util/PinList.h"        // parsePinList: the domain-neutral "18,19,20" CSV parser (core primitive)

#include <cstdint>
#include <cstdio>
#include <cstring>   // strcmp / strstr: match control names to a role

namespace mm {

/// The device's pin ownership map: which module owns each GPIO, for what, and whether it is safe.
///
/// It is keyed by physical GPIO, the way an OS device manager or a board template is.
/// A fixed System module, wired by code, and read-only.
/// Header-only: a small read-only diagnostic over the live tree and one platform seam.
///
/// @moreinfo
///
/// ## What it collects
///
/// It walks the live tree for every claimed pin, both pin controls and driver lane lists.
/// Each claim carries a severity flag and the live state read off the pad.
/// A disabled module's pins drop out, so the map reflects release-on-disable.
///
/// Each row is one GPIO with its owner and role.
/// A GPIO claimed twice shows the first owner and lists every claimant in the detail.
/// It surfaces rather than arbitrates, the controls already being the registry.
///
/// ## Why it only observes
///
/// MoonLight's equivalent is a central manager owning a table and assigning pins.
/// This inverts that: each module owns its pins, and one module observes them.
/// What is borrowed is the per-pin report rather than the ownership.
/// The role is derived from the control name, with no central vocabulary.
class PinsModule : public MoonModule {
public:
    /// Declare the one list of claimed pins.
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addList("pins", pins_);
    }

    /// Rebuild the claim map once a second, each claim copying what it needs into its own storage.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        pins_.refresh();
    }

private:
    /// The map itself, a source over a fixed snapshot, keeping every claim so conflicts show.
    struct PinListSource : ListSource {
        bool persistsList() const override { return false; }   // the pin map is rebuilt from the tree
        // Enough for any realistic board, and a diagnostic stops adding rather than allocating.
        static constexpr uint8_t kMaxClaims = 64;
        /// One control's claim on one pin, with everything the row needs copied in.
        struct Claim {
            uint8_t gpio;         ///< the physical GPIO, which keys the row
            // Copied, since a delete frees the module and the UI serializes right after one.
            char owner[16];       ///< the owning module's name
            char role[14];        ///< the derived role, per claim so lanes do not collapse
            const char* severity; ///< a static label, or null when the claim is safe
            const char* reason;   ///< why it is flagged, shown when the row expands
            platform::GpioLiveState live;  ///< direction, level and drive, sampled per refresh
        };
        Claim claims_[kMaxClaims];   ///< the snapshot
        uint8_t count_ = 0;          ///< how many claims it holds

        /// Re-walk the tree, order the claims by GPIO, then flag the conflicts.
        void refresh() {
            count_ = 0;
            Scheduler* s = Scheduler::instance();
            const uint8_t mc = s ? s->moduleCount() : 0;
            for (uint8_t i = 0; i < mc; i++)
                collect(s->module(i));
            // Ascending, so the map reads like a device manager.
            insertionSort(claims_, count_, [](const Claim& a, const Claim& b) { return a.gpio < b.gpio; });
            flagConflicts();
        }

        /// Flag every GPIO claimed twice, which surfaces a conflict without wedging the device.
        void flagConflicts() {
            uint8_t i = 0;
            while (i < count_) {
                uint8_t j = i + 1;
                while (j < count_ && claims_[j].gpio == claims_[i].gpio) j++;
                if (j - i >= 2)   // a run of >= 2 claims on the same GPIO
                    for (uint8_t k = i; k < j; k++) {
                        claims_[k].severity = "error";
                        claims_[k].reason = "claimed by 2+ controls: only one can drive this pin";
                    }
                i = j;
            }
        }

        uint8_t listRowCount() const override { return count_; }

        void writeListRow(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const Claim& c = claims_[row];
            sink.appendf("{\"gpio\":%u,\"owner\":", static_cast<unsigned>(c.gpio));
            sink.writeJsonString(c.owner);
            sink.append(",\"role\":");
            sink.writeJsonString(c.role);
            // Only when unsafe: the UI keys its row color on this, so a safe pin omits it.
            if (c.severity) {
                sink.append(",\"severity\":");
                sink.writeJsonString(c.severity);
            }
            // Omitted where the pad is not readable, so the row has no live fields.
            if (c.live.valid) {
                sink.append(",\"dir\":");
                sink.writeJsonString(dirLabel(c.live));
                sink.append(",\"level\":");
                sink.writeJsonString(c.live.level ? "HIGH" : "LOW");
                sink.append(",\"drive\":");
                sink.writeJsonString(driveLabel(c.live.driveCap));
            }
            sink.append("}");
        }

        /// The drive capability as a readable label, where a pin under-driving a long strip shows.
        static const char* driveLabel(uint8_t cap) {
            switch (cap) {
                case 0:  return "WEAK";
                case 1:  return "MEDIUM";
                case 2:  return "STRONG";
                default: return "STRONGEST";
            }
        }

        /// The pad's actual direction rather than the role's intent, "off" meaning neither buffer.
        static const char* dirLabel(const platform::GpioLiveState& live) {
            if (live.output && live.input) return "both";
            if (live.output) return "out";
            if (live.input)  return "in";
            return "off";
        }

        /// Append every claim on this GPIO, and why the row is flagged when it is.
        void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const uint8_t gpio = claims_[row].gpio;
            sink.append("{\"claims\":[");
            bool first = true;
            const char* reason = nullptr;
            for (uint8_t i = 0; i < count_; i++) {
                if (claims_[i].gpio != gpio) continue;
                if (!first) sink.append(",");
                first = false;
                char line[80];
                std::snprintf(line, sizeof(line), "%s \xC2\xB7 %s", claims_[i].owner, claims_[i].role);
                sink.writeJsonString(line);
                if (claims_[i].reason) reason = claims_[i].reason;   // the flag reason for this GPIO
            }
            sink.append("]");
            if (reason) {
                sink.append(",\"warning\":");
                sink.writeJsonString(reason);
            }
            sink.append("}");
        }

    private:
        /// Record every claimed pin in this subtree, depth first.
        void collect(MoonModule* m) {
            if (!m) return;
            // The lifecycle router's own predicate, so the map cannot show a pin it already freed.
            const bool active = m->effectivelyEnabled();
            const ControlList& cl = m->controls();
            for (uint8_t i = 0; active && i < cl.count(); i++) {
                const ControlDescriptor& d = cl[i];
                // Hidden means the peripheral is not using that pin, so it is free.
                if (d.hidden) continue;
                if (d.type == ControlType::Pin) {
                    const int8_t v = *static_cast<int8_t*>(d.ptr);
                    if (v >= 0) addPinClaim(static_cast<uint8_t>(v), m->name(), roleFor(d.name));
                } else if (d.type == ControlType::Text && isPinListName(d.name)) {
                    // One claim per pin, the parser deduping and rejecting what is out of range.
                    uint16_t pins[kMaxClaims];
                    uint8_t n = 0;
                    if (!parsePinList(static_cast<const char*>(d.ptr), pins, kMaxClaims, n))
                        for (uint8_t p = 0; p < n; p++)
                            addLaneClaim(static_cast<uint8_t>(pins[p]), m->name(), d.name, p);
                }
            }
            // The silicon-fixed pads, asked of the module since they are its own facts.
            if (active) {
                MoonModule::FixedPin fixed[16];
                const uint8_t n = m->fixedPins(fixed, 16);
                for (uint8_t i = 0; i < n; i++) addPinClaim(fixed[i].gpio, m->name(), fixed[i].role);
            }
            // Always recurse: a child is judged on its own enabled state.
            for (uint8_t i = 0; i < m->childCount(); i++)
                collect(m->child(i));
        }

        /// Take the next claim slot, sampling the live pad, or null once the map is full.
        Claim* reserve(uint8_t gpio, const char* owner) {
            if (count_ >= kMaxClaims) return nullptr;   // diagnostic: drop past the cap, never allocate
            Claim& c = claims_[count_++];
            c.gpio = gpio;
            c.severity = nullptr;
            c.reason = nullptr;
            c.live = platform::gpioLiveState(gpio);   // off the hot path, on the slow tick
            std::snprintf(c.owner, sizeof(c.owner), "%s", owner ? owner : "");   // always NUL-terminates
            return &c;
        }
        /// Record one pin control's claim, graded against the pin's capability.
        void addPinClaim(uint8_t gpio, const char* owner, const char* role) {
            if (Claim* c = reserve(gpio, owner)) {
                std::strncpy(c->role, role, sizeof(c->role) - 1);
                c->role[sizeof(c->role) - 1] = '\0';
                gradeClaim(*c, isOutputRole(role));
            }
        }
        /// Whether this control name holds a pin list, which is any name ending in "pins".
        static bool isPinListName(const char* name) {
            const size_t n = std::strlen(name);
            if (n < 4) return false;
            // Case-insensitive, since the convention is camelCase and a missed claim is invisible.
            const char* s = name + n - 4;
            return (s[0] == 'p' || s[0] == 'P') && s[1] == 'i' && s[2] == 'n' && s[3] == 's';
        }

        /// Record one entry of a pin list, named after the control it came from.
        void addLaneClaim(uint8_t gpio, const char* owner, const char* control, uint8_t laneIdx) {
            if (Claim* c = reserve(gpio, owner)) {
                // Named after the control, so a row says what the pin is.
                const bool isLed = std::strcmp(control, "pins") == 0;
                std::snprintf(c->role, sizeof(c->role), isLed ? "LED lane %u" : "relay %u",
                              static_cast<unsigned>(laneIdx));
                gradeClaim(*c, /*isOutput=*/true);   // both drive the pin
            }
        }

        /// Grade one claim against the pin's capability, setting its severity and its reason.
        // Reserved or invalid is an error; a driven role on a strap is a warning.
        static void gradeClaim(Claim& c, bool isOutput) {
            const platform::GpioCapability cap = platform::gpioCapability(c.gpio);
            if (!cap.validGpio)      { c.severity = "error"; c.reason = "invalid GPIO: not a pin on this chip"; return; }
            if (cap.reserved)        { c.severity = "error"; c.reason = "reserved for flash / PSRAM / USB"; return; }
            if (isOutput && !cap.outputCapable) { c.severity = "warn"; c.reason = "input-only pin: a driven role can't output here"; return; }
            if (isOutput && cap.strap)          { c.severity = "warn"; c.reason = "boot strap: driving it at reset can change boot mode"; return; }
        }

        /// Whether this role drives the pin, defaulting to yes so an unknown one errs toward warning.
        static bool isOutputRole(const char* role) {
            // The input roles, everything else being treated as driven.
            return !(std::strcmp(role, "data") == 0 || std::strcmp(role, "BCLK") == 0 ||
                     std::strcmp(role, "WS") == 0   || std::strcmp(role, "MCLK") == 0 ||
                     std::strcmp(role, "loopback Rx") == 0);
        }

        /// The role a control name implies, falling through to the name itself when unmatched.
        static const char* roleFor(const char* name) {
            struct Entry { const char* suffix; const char* role; };
            static constexpr Entry kRoles[] = {
                {"sckPin",        "BCLK"},        {"wsPin",         "WS"},
                {"sdPin",         "data"},        {"mclkPin",       "MCLK"},
                {"loopbackTxPin", "loopback Tx"}, {"loopbackRxPin", "loopback Rx"},
                {"ethMdcGpio",    "MDC"},         {"ethMdioGpio",   "MDIO"},
                {"sda",           "I\xC2\xB2""C SDA"}, {"scl",       "I\xC2\xB2""C SCL"},
            };
            for (const Entry& e : kRoles)
                if (std::strcmp(name, e.suffix) == 0) return e.role;
            return name;   // fall through to the control name: still informative
        }
    };

    PinListSource pins_;
};

} // namespace mm
