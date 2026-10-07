// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @defgroup MoonStats Consent, and the once-per-install trigger
/// @{
///
/// It owns what decides whether a report is built, building none itself and sending none.
/// Consent is the gate, and declining is silent: no connection opens and no identifier is computed.
///
/// @moreinfo
///
/// The trigger is a version comparison rather than a timer.
/// The reported version persists, so a reboot sends nothing and an upgrade sends one.
/// An empty value means a fresh install and a different one an upgrade.
/// No identifier is involved in telling those apart.
///
/// Reporting is suppressed in AP mode, where there is no route out.
/// See the privacy policy for what is promised.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/module/MoonModule.h"
#include "core/system/MoonCloudModule.h"
#include "core/module/Control.h"
#include "core/system/FilesystemModule.h"   // noteDirty(): scheduling the save, not just marking it
#include "core/util/JsonSink.h"
#include "core/module/Scheduler.h"
#include "core/util/build_info.h"
#include "platform/platform.h"
#include "core/util/LightOutput.h"           // lightSummary: the real light total the light domain publishes
#include "core/util/ModuleFactory.h"      // displayNameFor: the TYPE label, not the instance name
#include "core/moonlive/MoonLiveScriptFile.h"  // isFactoryScript: a shipped name is ours, not yours

namespace mm {

// A pure function over the live tree, allowlisted: emitting what it found would leak on run one.

/// What the report says happened, the first two automatic and the third a button press.
enum class MoonStatsEvent : uint8_t { Install, Upgrade, Refresh };

/// Read one control by name, so a rename drops the field rather than emitting its neighbor.
inline bool readControl(const MoonModule* mod, const char* name, JsonSink& out) {
    if (!mod) return false;
    auto& ctrls = mod->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        auto& c = ctrls[i];
        if (c.name && std::strcmp(c.name, name) == 0) {
            writeControlValue(out, c);
            return true;
        }
    }
    return false;
}

/// Find a module by name among the roots and their direct children.
inline const MoonModule* findModule(MoonModule* const* root, uint8_t count, const char* name) {
    for (uint8_t i = 0; i < count; i++) {
        const MoonModule* m = root[i];
        if (!m) continue;
        if (m->name() && std::strcmp(m->name(), name) == 0) return m;
        for (uint8_t c = 0; c < m->childCount(); c++) {
            if (const MoonModule* ch = m->child(c)) {
                if (ch->name() && std::strcmp(ch->name(), name) == 0) return ch;
            }
        }
    }
    return nullptr;
}

/// Copy one named control into the report, omitting the key when the control is absent.
inline void field(JsonSink& sink, const MoonModule* mod, const char* control, const char* key,
           bool& first) {
    if (!mod) return;
    JsonSink value;
    if (!readControl(mod, control, value)) return;
    if (!first) sink.append(",");
    first = false;
    sink.append("\"");
    sink.append(key);
    sink.append("\":");
    sink.append(value.data());
}


/// The shipped script a module runs, or null for one a user wrote or none at all.
inline const char* factoryScriptOf(const MoonModule* m) {
    if (!m) return nullptr;
    const ControlList& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (cs[i].type != ControlType::FilePath || !cs[i].name) continue;
        if (std::strcmp(cs[i].name, "script") != 0) continue;
        const char* value = static_cast<const char*>(cs[i].ptr);
        return moonlive::isFactoryScript(value) ? value : nullptr;
    }
    return nullptr;
}

/// Append every user-added enabled module, depth first, each as its role and name.
inline void reportModules(JsonSink& sink, const MoonModule* const* mods, uint8_t count,
                          bool& first) {
    for (uint8_t i = 0; i < count; i++) {
        const MoonModule* m = mods[i];
        if (!m || !m->enabled()) continue;
        // Role decides: Generic and Layer are structural, so what remains is what somebody chose.
        const ModuleRole role = m->role();
        // Boot wiring rather than a choice, excluded by type since the wired flag marks children.
        const bool prewired = std::strcmp(m->typeName(), "PreviewDriver") == 0;
        if (m->name() && !prewired && role != ModuleRole::Generic && role != ModuleRole::Layer) {
            // Reported only when we ship it, since a name the user invented is text they typed.
            const char* script = factoryScriptOf(m);
            // The type, not the instance name, which is user-editable and carries a suffix.
            const char* label = m->typeName() && m->typeName()[0]
                                    ? ModuleFactory::displayNameFor(m->typeName(), role)
                                    : m->name();
            char entry[80];
            if (script)
                std::snprintf(entry, sizeof(entry), "%s:%s/%s", roleName(role), label, script);
            else
                std::snprintf(entry, sizeof(entry), "%s:%s", roleName(role), label);
            if (!first) sink.append(",");
            first = false;
            sink.writeJsonString(entry);
        }
        for (uint8_t c = 0; c < m->childCount(); c++) {
            const MoonModule* child = m->child(c);
            reportModules(sink, &child, 1, first);
        }
    }
}

inline void buildMoonStatsReport(JsonSink& sink,
                          MoonModule* const* root, uint8_t moduleCount,
                          MoonStatsEvent event,
                          const char* installationId,
                          const char* version,
                          const char* previousVersion,
                          uint32_t lightCount = 0,
                          uint32_t totalHeap = 0, uint32_t freeHeap = 0,
                          uint32_t fps = 0) {
    const MoonModule* system = findModule(root, moduleCount, "System");

    sink.append("{");
    bool first = true;

    // Omitted without consent, and by the test asserting the forbidden fields.
    if (installationId && *installationId) {
        sink.append("\"installationId\":");
        sink.writeJsonString(installationId);
        first = false;
    }

    if (!first) sink.append(",");
    sink.append("\"event\":");
    sink.writeJsonString(event == MoonStatsEvent::Refresh ? "refresh"
                         : event == MoonStatsEvent::Upgrade ? "upgrade" : "install");
    first = false;

    if (version && *version) {
        sink.append(",\"version\":");
        sink.writeJsonString(version);
    }

    // Set by CI and empty locally, so it describes the binary rather than the person.
    sink.append(",\"dev\":");
    sink.writeBool(kRelease[0] == 0);
    // Present only on an upgrade, and it is what tells the server this was one.
    if (previousVersion && *previousVersion) {
        sink.append(",\"previousVersion\":");
        sink.writeJsonString(previousVersion);
    }

    // Raw, since bucketing here would freeze every stored row at today's boundaries.
    sink.appendf(",\"totalHeap\":%u,\"freeHeap\":%u,\"lightCount\":%u,\"fps\":%u",
                 static_cast<unsigned>(totalHeap),
                 static_cast<unsigned>(freeHeap),
                 static_cast<unsigned>(lightCount),
                 static_cast<unsigned>(fps));

    // Facts about the board: the name and address sit in the same list and are deliberately absent.
    field(sink, system, "chip", "chip", first);
    field(sink, system, "flash", "flash", first);
    field(sink, system, "psramType", "psram", first);
    field(sink, system, "sdk", "sdk", first);
    field(sink, system, "deviceModel", "deviceModel", first);

    // What the user chose to run, from the catalog's fixed vocabulary.
    sink.append(",\"modules\":[");
    bool firstModule = true;
    reportModules(sink, root, moduleCount, firstModule);
    sink.append("]");

    sink.append("}");
    sink.flush();
}


class MoonStatsModule : public MoonModule {
public:
    /// Read the running version, then publish the consent explanation.
    void setup() override {
        std::snprintf(runningVersion_, sizeof(runningVersion_), "%s", kVersion);
        refreshStatus();
        MoonModule::setup();
    }

    /// Say what this setting exchanges, on the status slot rather than in a control of its own.
    void refreshStatus() {
        if (consent_) clearStatus();
        else setStatus("Off. Switch on to share what hardware you run, once per install or upgrade "
                       "and whenever you press send update: "
                       "the empty charts below are exactly what it contributes to, and what you get "
                       "back. No device name, no addresses, no credentials.");
    }

    /// Refresh the explanation on a consent change, and send on the button.
    void onControlChanged(const char* name) override {
        if (!name) return;
        if (std::strcmp(name, "consent") == 0) { refreshStatus(); return; }
        if (std::strcmp(name, "send update") != 0) return;
        // Each line below describes the last attempt, so a stale one is a lie once the next starts.
        clearOwnStatus();
        // Every outcome says something, and consent is re-read since a write can arrive from the API.
        if (!consent_) { setOwnStatus("Switch consent on first: nothing is sent while it is off.",
                                      Severity::Warning); return; }
        if (!platform::httpsAvailable()) {
            setOwnStatus("This build cannot send: it was compiled without an HTTPS client.",
                         Severity::Error);
            return;
        }
        if (!platform::networkReady()) {
            setOwnStatus("No network yet. Press send update again once this device is online.",
                         Severity::Warning);
            return;
        }
        if (inApMode()) {
            setOwnStatus("Serving its own access point, so there is no route out. "
                         "Join a network, then press send update.", Severity::Warning);
            return;
        }
        if (sendReport(MoonStatsEvent::Refresh)) {
            setOwnStatus("Sent. The charts below now describe this device as it is now.",
                         Severity::Status);
        } else {
            setOwnStatus("Could not reach the server. Nothing was sent; press send update to try again.",
                         Severity::Error);
        }
    }

    /// Declare the consent, the two version readouts and the send button.
    void defineControls() override {
        controls_.clear();
        // A checkbox, since "not now" and "never" both mean nothing is sent.
        controls_.addControl("consent", consent_);

        // Text with the readonly flag, since persistence refuses a read-only control.
        controls_.addText("reportedVersion", reportedVersion_, sizeof(reportedVersion_));
        controls_.setReadOnly(controls_.count() - 1, true);

        // Derived at setup from a constant, so it has nothing to persist.
        controls_.addReadOnly("version", runningVersion_, sizeof(runningVersion_));

        // For a setup that changed without a version change, replacing the row rather than adding.
        controls_.addButton("send update");
    }

    /// Whether a report is due: consent given, and the running version differs from the reported.
    bool reportDue() const {
        if (!consent_) return false;
        return std::strcmp(reportedVersion_, runningVersion_) != 0;
    }

    /// Which kind of report is due.
    MoonStatsEvent dueEvent() const {
        return reportedVersion_[0] == 0 ? MoonStatsEvent::Install : MoonStatsEvent::Upgrade;
    }

    /// The version being replaced, for an upgrade report, or nullptr on a fresh install.
    const char* previousVersion() const {
        return reportedVersion_[0] == 0 ? nullptr : reportedVersion_;
    }

    /// Record that a report was sent, on hand-off rather than on success.
    void markReported() {
        std::snprintf(reportedVersion_, sizeof(reportedVersion_), "%s", runningVersion_);
        // Marking alone does not schedule a save, and the pair is what a control write does.
        markDirty();
        FilesystemModule::noteDirty();
    }

    /// Record the user's answer, the same way a control write does.
    void setConsent(bool yes) { consent_ = yes; markDirty(); refreshStatus(); }

    /// Whether the user has agreed to report.
    bool consent() const { return consent_; }

    /// Fake the running version, which setup reads from a constant a test cannot change.
    void setRunningVersionForTest(const char* v) {
        std::snprintf(runningVersion_, sizeof(runningVersion_), "%s", v ? v : "");
    }

    /// This installation's id, or empty without consent, so no caller can obtain one to log.
    void installationId(char* out) const {
        if (!out) return;
        if (!consent_) { out[0] = 0; return; }
        mm::installationId(out);
    }

    /// Send the automatic report once every guard below passes.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        if (!reportDue()) return;
        // Never sends, so there is nothing to hand off and nothing to mark.
        if (!platform::httpsAvailable()) return;
        if (!platform::networkReady()) return;   // try again next second
        // Our own AP means no route out, so a send would fail and mark itself reported.
        if (inApMode()) return;
        // The rate is computed only once the first timing window closes, so waiting costs one second.
        auto* sched = Scheduler::instance();
        if (!sched || sched->fps() == 0) return;
        sendReport(dueEvent());
    }

    /// Whether we serve our own access point, asked of the platform so no copy goes stale.
    bool inApMode() const { return platform::wifiApConnected(); }

private:
    /// Build the report and post it once, marking it reported on hand-off.
    bool sendReport(MoonStatsEvent kind) {
        char id[kInstallationIdChars + 1] = {};
        installationId(id);
        if (!id[0]) return false;   // no consent, no id, no report

        // The scheduler's own capacity, so this cannot truncate a tree it accepted.
        MoonModule* tree[32] = {};
        auto* sched = Scheduler::instance();
        if (!sched) return false;
        uint8_t count = 0;
        for (uint8_t i = 0; i < sched->moduleCount() && count < 32; i++) {
            if (MoonModule* m = sched->module(i)) tree[count++] = m;
        }

        JsonSink body;
        // Only on an upgrade: the server reads its presence as what makes a row one.
        const char* prev = (kind == MoonStatsEvent::Upgrade) ? previousVersion() : nullptr;
        buildMoonStatsReport(body, tree, count,
                             kind, id, runningVersion_, prev,
                             lightSummary().lightCount,
                             static_cast<uint32_t>(platform::totalHeap()),
                             static_cast<uint32_t>(platform::freeHeap()),
                             sched->fps());

        // Through the container, which owns the address. Whether it was accepted is reported.
        bool sent = false;
        if (auto* cloud = static_cast<const MoonCloudModule*>(parent())) {
            sent = cloud->post("/api/report", body.data());
        }
        // The automatic report marks itself either way; a failed press must not.
        if (kind != MoonStatsEvent::Refresh || sent) markReported();
        return sent;
    }

    /// The verdict this module last set, or null, so the next press retracts exactly this one.
    const char* ownStatus_ = nullptr;

protected:
    // A subclass tool and a test seam, never part of the card's public surface.

    /// Set a verdict and remember it, so the next press can retract exactly this one.
    void setOwnStatus(const char* msg, Severity sev) {
        ownStatus_ = msg;
        setStatus(msg, sev);
    }

    /// Retract this module's verdict, and only this module's.
    void clearOwnStatus() {
        if (ownStatus_) {
            if (status() == ownStatus_) clearStatus();
            ownStatus_ = nullptr;
        }
    }

private:
    bool consent_ = false;             ///< whether the user has agreed to report
    char reportedVersion_[32] = {};    ///< the version that last reported
    char runningVersion_[32] = {};     ///< the version running now
};

/// @}

}  // namespace mm
